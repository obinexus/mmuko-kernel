// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Candidate selection: consensus voting, artifact confidence, SemVerX weight.
//!
//! # Where this sits
//!
//! [`crate::trident`] answers a yes/no question about ONE candidate: may this
//! provider fill this slot? Selection answers a different question: given
//! SEVERAL providers that could fill it, which one should we try?
//!
//! ```text
//!   candidates ──▶ ABI GATE ──▶ survivors ──▶ SCORE ──▶ pick highest
//!                (trident)                  (this file)
//! ```
//!
//! **The gate runs first, and this is not an implementation detail.** A
//! candidate with a perfect artifact score, unanimous Yes votes and the
//! highest version in the registry scores exactly nothing if its fingerprint
//! disagrees with the call site, because it never enters the scored set at
//! all. If the order were reversed -- score everything, then check the winner
//! -- a high-scoring broken candidate would beat a low-scoring correct one and
//! the resolver would report "no viable provider" while a working one sat
//! unexamined. Worse, a weighted scheme with a "compatibility" term in it
//! could be tuned until a broken candidate wins, and someone eventually would.
//!
//! So: shape is a filter, never a term.
//!
//! # Fixed point, not floating point
//!
//! Scores are `u32` in units of one millionth ([`SCORE_ONE`]) rather than
//! `f64`. Three reasons, all of them kernel-specific:
//!
//! 1. At ring 0 the FPU/SSE register file is usually not saved on entry. A
//!    resolver that touches XMM registers either corrupts userspace state or
//!    forces an expensive save on every call into it.
//! 2. `f64` arithmetic is not bit-reproducible across compilers, optimisation
//!    levels and FMA availability. Two nodes that must agree on a ranking
//!    would be comparing numbers that could differ in the last place.
//! 3. Ordering `f64` requires handling NaN. A `NaN` artifact score in a
//!    `partial_cmp` chain silently loses every comparison, so a corrupt input
//!    would quietly rank last instead of being rejected.
//!
//! The arithmetic below uses `u64` intermediates and saturating operations, so
//! it cannot overflow or panic on any input.

use crate::abi::{Arch, ExportDesc};
use crate::kernel::Policy;
use crate::semverx::{SemVerX, State};
use crate::trident::{BindState, Trident};

/// Fixed-point scale: this value represents 1.0.
pub const SCORE_ONE: u32 = 1_000_000;

/// A consensus vote on a candidate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Vote {
    /// Endorsed.
    Yes,
    /// Rejected.
    No,
    /// Abstained. Counted as neutral, not as absent -- an abstention is
    /// information (someone looked and would not commit), and dropping it
    /// would let one Yes among nine abstentions read as unanimous.
    Nil,
}

impl Vote {
    /// The vote's value in fixed point.
    pub const fn value(self) -> u32 {
        match self {
            Vote::Yes => SCORE_ONE,
            Vote::Nil => SCORE_ONE / 2,
            Vote::No => 0,
        }
    }
}

/// A tally of votes. Stored as counts rather than a list so a candidate is
/// `Copy` and fixed-size, which keeps the whole graph allocation-free.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Votes {
    /// Endorsements.
    pub yes: u16,
    /// Rejections.
    pub no: u16,
    /// Abstentions.
    pub nil: u16,
}

impl Votes {
    /// An empty tally.
    pub const fn none() -> Votes {
        Votes { yes: 0, no: 0, nil: 0 }
    }

    /// A tally from explicit counts.
    pub const fn new(yes: u16, no: u16, nil: u16) -> Votes {
        Votes { yes, no, nil }
    }

    /// Record one vote.
    pub fn cast(&mut self, v: Vote) {
        match v {
            Vote::Yes => self.yes = self.yes.saturating_add(1),
            Vote::No => self.no = self.no.saturating_add(1),
            Vote::Nil => self.nil = self.nil.saturating_add(1),
        }
    }

    /// Total votes cast.
    pub const fn total(&self) -> u32 {
        self.yes as u32 + self.no as u32 + self.nil as u32
    }

    /// Consensus score in `[0, SCORE_ONE]`.
    ///
    /// An empty tally scores neutral rather than zero: "nobody has looked at
    /// this" is not the same claim as "everybody who looked rejected it", and
    /// scoring them identically would make a brand-new correct release
    /// indistinguishable from a condemned one.
    pub fn consensus(&self) -> u32 {
        let n = self.total();
        if n == 0 {
            return SCORE_ONE / 2;
        }
        let sum = (self.yes as u64) * (SCORE_ONE as u64)
            + (self.nil as u64) * (SCORE_ONE as u64 / 2);
        (sum / n as u64) as u32
    }
}

/// Maturity multiplier for a version's major state, in fixed point.
///
/// Stable and LTS are unweighted; everything else is discounted. This is a
/// PREFERENCE, not a safety property: a discount cannot make an unsafe
/// candidate safe, and the policy mask in [`Policy`] is what actually excludes
/// states a deployment will not accept.
pub const fn state_multiplier(s: State) -> u32 {
    match s {
        State::Stable | State::Lts => SCORE_ONE,
        State::Legacy => 800_000,
        State::Beta => 750_000,
        State::Experimental => 600_000,
        State::Invalid => 0,
    }
}

/// Version weight in `[0, SCORE_ONE]`.
///
/// `base = major*100 + minor*10 + patch`, compressed by `base / (base + 100)`
/// so that the scale saturates instead of exploding, then multiplied by the
/// major state's maturity discount.
///
/// The compression matters: without it a package at major 400 would outweigh
/// every other term in the combined score put together, and the ranking would
/// become "whoever bumped their major number most often".
pub fn semver_weight(v: &SemVerX) -> u32 {
    let base = (v.major as u64) * 100 + (v.minor as u64) * 10 + (v.patch as u64);
    let w = if base == 0 {
        0u64
    } else {
        (base * SCORE_ONE as u64) / (base + 100)
    };
    let m = state_multiplier(v.major_state) as u64;
    ((w * m) / SCORE_ONE as u64) as u32
}

/// Relative weights of the three score terms, in tenths. Must sum to 10.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Weights {
    /// Consensus term.
    pub consensus: u32,
    /// Artifact confidence term.
    pub artifact: u32,
    /// Version weight term.
    pub semver: u32,
}

impl Default for Weights {
    /// 50% consensus, 30% artifact, 20% version.
    fn default() -> Self {
        Weights {
            consensus: 5,
            artifact: 3,
            semver: 2,
        }
    }
}

impl Weights {
    /// Whether the weights sum to 10. Checked by [`Candidate::score`], which
    /// falls back to the default rather than producing a score on a scale
    /// nobody can interpret.
    pub const fn is_normalised(&self) -> bool {
        self.consensus + self.artifact + self.semver == 10
    }
}

/// One candidate provider for a slot.
#[derive(Debug, Clone, Copy)]
pub struct Candidate {
    /// Identifier, for diagnostics and tie-breaking.
    pub id: &'static str,
    /// The candidate's version.
    pub version: SemVerX,
    /// Build/provenance confidence in `[0, SCORE_ONE]`. Clamped on use.
    pub artifact_score: u32,
    /// Consensus tally.
    pub votes: Votes,
    /// The provider descriptor, if this candidate is actually loadable. `None`
    /// means the candidate is known about but not present, and it can never
    /// pass the gate.
    pub export: Option<ExportDesc>,
}

impl Candidate {
    /// A candidate with a neutral tally and no provider.
    pub const fn new(id: &'static str, version: SemVerX, artifact_score: u32) -> Candidate {
        Candidate {
            id,
            version,
            artifact_score,
            votes: Votes::none(),
            export: None,
        }
    }

    /// Combined score in `[0, SCORE_ONE]`.
    pub fn score(&self, w: Weights) -> u32 {
        let w = if w.is_normalised() { w } else { Weights::default() };
        let c = self.votes.consensus() as u64;
        let a = (self.artifact_score.min(SCORE_ONE)) as u64;
        let s = semver_weight(&self.version) as u64;
        ((w.consensus as u64 * c + w.artifact as u64 * a + w.semver as u64 * s) / 10) as u32
    }

    /// Does this candidate pass the ABI gate for `required` under `policy`?
    ///
    /// Runs the real resolver on a throwaway node rather than reimplementing
    /// the comparison. There is exactly one place in this system that decides
    /// whether two contracts agree, and selection is not permitted to become a
    /// second one.
    pub fn passes_gate(&self, required: &ExportDesc, policy: &Policy) -> bool {
        let export = match self.export {
            Some(e) => e,
            None => return false,
        };
        let mut probe = Trident::new("probe", *required);
        probe.swap(Some(export), policy, true) == BindState::Bound
    }

    /// The candidate's score if it passes the gate, `None` otherwise.
    pub fn gated_score(
        &self,
        required: &ExportDesc,
        policy: &Policy,
        w: Weights,
    ) -> Option<u32> {
        if self.passes_gate(required, policy) {
            Some(self.score(w))
        } else {
            None
        }
    }
}

/// Ordering used to pick a winner.
///
/// Score first; ties broken by version numbers, then by identifier. The
/// identifier tie-break is what makes the choice TOTAL: without it two
/// candidates that are equal on every measured axis would be separated by
/// whatever order they happened to be enumerated in, and the same registry
/// would resolve differently on two machines.
fn better(a: (&Candidate, u32), b: (&Candidate, u32)) -> bool {
    if a.1 != b.1 {
        return a.1 > b.1;
    }
    let av = (a.0.version.major, a.0.version.minor, a.0.version.patch);
    let bv = (b.0.version.major, b.0.version.minor, b.0.version.patch);
    if av != bv {
        return av > bv;
    }
    a.0.id > b.0.id
}

/// Pick the highest-scoring candidate that passes the gate.
///
/// Returns the index into `candidates`, or `None` if every candidate was
/// filtered out. `None` here means "no candidate can legally fill this slot",
/// which is a materially different report from "the best candidate scored
/// low", and the caller should surface it as such.
pub fn select<'a>(
    candidates: &'a [Candidate],
    required: &ExportDesc,
    policy: &Policy,
    weights: Weights,
) -> Option<usize> {
    let mut best: Option<(usize, &'a Candidate, u32)> = None;
    for (i, c) in candidates.iter().enumerate() {
        let score = match c.gated_score(required, policy, weights) {
            Some(s) => s,
            None => continue,
        };
        best = match best {
            None => Some((i, c, score)),
            Some((bi, bc, bs)) => {
                if better((c, score), (bc, bs)) {
                    Some((i, c, score))
                } else {
                    Some((bi, bc, bs))
                }
            }
        };
    }
    best.map(|(i, _, _)| i)
}

/// Why a candidate was excluded. For reporting a selection that found nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Excluded {
    /// The candidate has no loadable provider.
    NotPresent,
    /// The provider's contract disagrees with the call site.
    GateFailed,
}

/// Classify one candidate, for a diagnostic listing of a failed selection.
pub fn explain(c: &Candidate, required: &ExportDesc, policy: &Policy) -> Result<u32, Excluded> {
    if c.export.is_none() {
        return Err(Excluded::NotPresent);
    }
    if !c.passes_gate(required, policy) {
        return Err(Excluded::GateFailed);
    }
    Ok(c.score(Weights::default()))
}

// ---------------------------------------------------------------------------
// Graph traversal
// ---------------------------------------------------------------------------

/// Maximum children per node in a [`SelectGraph`].
pub const MAX_CHILDREN: usize = 8;

/// A fixed-capacity DAG of candidates, traversed by descending score.
///
/// The equivalent of a `HashMap` graph, sized at compile time so it lives in
/// static storage with no allocator. `N` nodes, at most [`MAX_CHILDREN`] edges
/// each.
pub struct SelectGraph<const N: usize> {
    nodes: [Option<Candidate>; N],
    children: [[u16; MAX_CHILDREN]; N],
    nchildren: [u8; N],
    len: usize,
}

impl<const N: usize> Default for SelectGraph<N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<const N: usize> SelectGraph<N> {
    /// An empty graph.
    pub const fn new() -> Self {
        SelectGraph {
            nodes: [None; N],
            children: [[0u16; MAX_CHILDREN]; N],
            nchildren: [0u8; N],
            len: 0,
        }
    }

    /// Add a node. Returns its index.
    pub fn add(&mut self, c: Candidate) -> Option<usize> {
        if self.len >= N {
            return None;
        }
        let i = self.len;
        self.nodes[i] = Some(c);
        self.nchildren[i] = 0;
        self.len += 1;
        Some(i)
    }

    /// Add an edge from `from` to `to`.
    pub fn edge(&mut self, from: usize, to: usize) -> bool {
        if from >= self.len || to >= self.len {
            return false;
        }
        let n = self.nchildren[from] as usize;
        if n >= MAX_CHILDREN {
            return false;
        }
        self.children[from][n] = to as u16;
        self.nchildren[from] = (n + 1) as u8;
        true
    }

    /// Look up a node by identifier.
    pub fn find(&self, id: &str) -> Option<usize> {
        (0..self.len).find(|&i| self.nodes[i].map(|c| c.id) == Some(id))
    }

    /// Read a node.
    pub fn get(&self, i: usize) -> Option<&Candidate> {
        self.nodes.get(i)?.as_ref()
    }

    /// Walk from `start`, at each step taking the highest-scoring child that
    /// passes the gate, until a leaf or a dead end.
    ///
    /// Writes the path into `out` and returns how many entries were written.
    ///
    /// Visited nodes are tracked so a cycle terminates the walk rather than
    /// looping. A cycle in a dependency graph is a defect, but a resolver that
    /// hangs on one is a worse defect: the walk stops and the caller sees a
    /// short path, which is diagnosable.
    pub fn best_path(
        &self,
        start: usize,
        required: &ExportDesc,
        policy: &Policy,
        weights: Weights,
        out: &mut [usize],
    ) -> usize {
        let mut visited = [false; N];
        let mut cur = start;
        let mut n = 0usize;

        while n < out.len() && cur < self.len && !visited[cur] {
            visited[cur] = true;
            out[n] = cur;
            n += 1;

            let mut best: Option<(usize, u32)> = None;
            for k in 0..self.nchildren[cur] as usize {
                let ci = self.children[cur][k] as usize;
                let child = match self.get(ci) {
                    Some(c) => c,
                    None => continue,
                };
                let score = match child.gated_score(required, policy, weights) {
                    Some(s) => s,
                    None => continue,
                };
                best = match best {
                    None => Some((ci, score)),
                    Some((bi, bs)) => {
                        let bc = self.get(bi).unwrap();
                        if better((child, score), (bc, bs)) {
                            Some((ci, score))
                        } else {
                            Some((bi, bs))
                        }
                    }
                };
            }

            match best {
                Some((next, _)) => cur = next,
                None => break,
            }
        }
        n
    }
}

/// Parse a compact `major.minor.patch-channel` version string into SemVerX.
///
/// This is an INPUT DIALECT, not the canonical form. It exists because that is
/// how versions arrive from npm-shaped registries and from human hands, and
/// refusing to read them would just mean somebody writes a lossy converter
/// elsewhere.
///
/// The single channel is applied to all three state fields, and the absence of
/// one is an error rather than a default of "stable". That last point is the
/// difference from an ordinary SemVer parser and it is deliberate: guessing a
/// state at the parser is exactly the ambiguity SemVerX exists to remove, and
/// a guess made here would be indistinguishable downstream from a state
/// somebody actually declared.
///
/// [`SemVerX::parse`] remains the canonical six-field reader.
pub fn parse_compact(s: &str) -> Option<SemVerX> {
    let (nums, channel) = s.split_once('-')?;
    let state = match channel {
        "lts" => State::Lts,
        other => State::parse(other)?,
    };
    let mut it = nums.split('.');
    let major = it.next()?.parse::<u16>().ok()?;
    let minor = it.next()?.parse::<u16>().ok()?;
    let patch = it.next()?.parse::<u16>().ok()?;
    if it.next().is_some() {
        return None;
    }
    Some(SemVerX::uniform(major, minor, patch, state))
}

/// The profile a selection is being made for. Re-exported for callers that
/// build a [`Policy`] inline.
pub use crate::abi::Arch as SelectArch;

const _: () = assert!(SCORE_ONE == 1_000_000);
const _: fn() = || {
    // Compile-time reminder that Arch is part of the gate, not the score.
    let _ = Arch::Mmuko64;
};
