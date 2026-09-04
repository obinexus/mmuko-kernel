// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! Candidate selection: consensus votes, artifact confidence, version weight,
//! and the ABI gate that runs before any of them.
//!
//! The property this file exists to pin down is the ORDERING. Scoring ranks
//! survivors; it never admits anything. A candidate that would win every
//! scoring contest must still lose to a worse-scoring one if its contract
//! disagrees with the call site, because it is not in the contest at all.

use mmuko_kernel::abi::{ty, CallConv, ExportDesc, FnSig};
use mmuko_kernel::kernel::Policy;
use mmuko_kernel::select::{
    self, parse_compact, Candidate, SelectGraph, Vote, Votes, Weights, SCORE_ONE,
};
use mmuko_kernel::semverx::{SemVerX, State};

extern "C" fn add_i32(a: i32, b: i32) -> i32 {
    a + b
}
extern "C" fn add_f64(a: f64, b: f64) -> f64 {
    a + b
}

static SIG_II_I: FnSig = FnSig::new(CallConv::Cdecl, &ty::I32, &[ty::I32, ty::I32]);
static SIG_DD_D: FnSig = FnSig::new(CallConv::Cdecl, &ty::F64, &[ty::F64, ty::F64]);

fn required() -> ExportDesc {
    ExportDesc::require("add", &SIG_II_I, SemVerX::uniform(1, 0, 0, State::Stable))
}

/// A provider that keeps the int32 `add` contract.
///
/// Note what `since` is, and is not. It is the version at which this SYMBOL'S
/// SHAPE appeared -- not the version of the module carrying it. A module may
/// have moved on to 3.stable while `add` has not changed shape since
/// 1.stable.0, and it is that 1.stable.0 that goes here. Passing the module's
/// own version instead would make every release outside major line 1 fault on
/// version, and the resolver would refuse correct providers for a reason that
/// has nothing to do with the machine contract.
fn good(_module_ver: SemVerX) -> ExportDesc {
    ExportDesc::provide(
        "add",
        &SIG_II_I,
        SemVerX::uniform(1, 0, 0, State::Stable),
        add_i32 as *const () as *mut _,
    )
}

/// A provider that changed `add` to take doubles. Its shape is new, so its
/// `since` is a new major line -- but the fingerprint refuses it long before
/// any version rule is consulted.
fn broken(_module_ver: SemVerX) -> ExportDesc {
    ExportDesc::provide(
        "add",
        &SIG_DD_D,
        SemVerX::uniform(2, 0, 0, State::Stable),
        add_f64 as *const () as *mut _,
    )
}

fn cand(
    id: &'static str,
    ver: SemVerX,
    artifact: u32,
    votes: Votes,
    export: Option<ExportDesc>,
) -> Candidate {
    Candidate {
        id,
        version: ver,
        artifact_score: artifact,
        votes,
        export,
    }
}

// --------------------------------------------------------------------------
// Scoring arithmetic
// --------------------------------------------------------------------------

#[test]
fn consensus_maps_votes_to_fixed_point() {
    assert_eq!(Votes::new(3, 0, 0).consensus(), SCORE_ONE, "unanimous yes");
    assert_eq!(Votes::new(0, 3, 0).consensus(), 0, "unanimous no");
    assert_eq!(Votes::new(0, 0, 3).consensus(), SCORE_ONE / 2, "all abstain");
    assert_eq!(Votes::new(2, 2, 0).consensus(), SCORE_ONE / 2, "evenly split");

    // The article's node A: Yes, Yes, Nil -> (1 + 1 + 0.5) / 3 = 0.8333...
    let a = Votes::new(2, 0, 1).consensus();
    assert!((833_000..=834_000).contains(&a), "got {}", a);

    // B1: Yes, Yes, Yes, No -> 0.75
    assert_eq!(Votes::new(3, 1, 0).consensus(), 750_000);
    // B2: Yes, Nil, No, No -> (1 + 0.5) / 4 = 0.375
    assert_eq!(Votes::new(1, 2, 1).consensus(), 375_000);
}

#[test]
fn an_empty_tally_is_neutral_not_zero() {
    // "Nobody has looked at this" is not the claim "everybody who looked
    // rejected it". Scoring them the same would make a new correct release
    // indistinguishable from a condemned one.
    assert_eq!(Votes::none().consensus(), SCORE_ONE / 2);
    assert_ne!(Votes::none().consensus(), Votes::new(0, 1, 0).consensus());
}

#[test]
fn votes_can_be_cast_incrementally() {
    let mut v = Votes::none();
    v.cast(Vote::Yes);
    v.cast(Vote::No);
    v.cast(Vote::Nil);
    assert_eq!(v, Votes::new(1, 1, 1));
    assert_eq!(v.total(), 3);
    assert_eq!(v.consensus(), SCORE_ONE / 2);
}

#[test]
fn semver_weight_compresses_and_discounts() {
    let stable = SemVerX::uniform(2, 0, 0, State::Stable);
    let legacy = SemVerX::uniform(2, 0, 0, State::Legacy);
    let experimental = SemVerX::uniform(2, 0, 0, State::Experimental);
    let lts = SemVerX::uniform(2, 0, 0, State::Lts);

    let ws = select::semver_weight(&stable);
    assert!(ws > 0 && ws <= SCORE_ONE);
    assert_eq!(select::semver_weight(&lts), ws, "LTS is not discounted");
    assert!(select::semver_weight(&legacy) < ws, "legacy is discounted");
    assert!(
        select::semver_weight(&experimental) < select::semver_weight(&legacy),
        "experimental is discounted further"
    );

    // Compression: without base/(base+100) a major-400 package would swamp
    // every other term and the ranking would become "who bumped most".
    let huge = SemVerX::uniform(400, 0, 0, State::Stable);
    assert!(
        select::semver_weight(&huge) <= SCORE_ONE,
        "the weight saturates rather than exploding"
    );
    assert!(
        select::semver_weight(&huge) - ws < SCORE_ONE / 2,
        "a 200x version bump is worth less than half the term's range"
    );

    assert_eq!(
        select::semver_weight(&SemVerX::uniform(0, 0, 0, State::Stable)),
        0,
        "0.0.0 carries no version weight"
    );
}

#[test]
fn scoring_never_overflows_or_panics() {
    // Saturating arithmetic on the extremes of every input.
    let extreme = cand(
        "x",
        SemVerX::uniform(u16::MAX, u16::MAX, u16::MAX, State::Stable),
        u32::MAX, // deliberately out of range; must be clamped, not wrapped
        Votes::new(u16::MAX, u16::MAX, u16::MAX),
        None,
    );
    let s = extreme.score(Weights::default());
    assert!(s <= SCORE_ONE, "score stayed in range: {}", s);
}

#[test]
fn unnormalised_weights_fall_back_to_the_default() {
    let c = cand(
        "c",
        SemVerX::uniform(1, 0, 0, State::Stable),
        SCORE_ONE,
        Votes::new(1, 0, 0),
        None,
    );
    let bad = Weights {
        consensus: 9,
        artifact: 9,
        semver: 9,
    };
    assert!(!bad.is_normalised());
    assert_eq!(
        c.score(bad),
        c.score(Weights::default()),
        "a score on an uninterpretable scale is worse than a default one"
    );
}

// --------------------------------------------------------------------------
// The ordering property
// --------------------------------------------------------------------------

#[test]
fn the_abi_gate_runs_before_scoring_not_after() {
    let req = required();
    let policy = Policy::default();

    // The strongest possible candidate on every scored axis -- unanimous
    // endorsement, perfect artifact confidence, highest version -- but its
    // `add` takes doubles.
    let perfect_but_broken = cand(
        "perfect",
        SemVerX::uniform(9, 9, 9, State::Stable),
        SCORE_ONE,
        Votes::new(100, 0, 0),
        Some(broken(SemVerX::uniform(9, 9, 9, State::Stable))),
    );
    // The weakest plausible candidate that is actually correct.
    let weak_but_correct = cand(
        "weak",
        SemVerX::uniform(1, 0, 0, State::Stable),
        1,
        Votes::new(0, 9, 1),
        Some(good(SemVerX::uniform(1, 0, 0, State::Stable))),
    );

    assert!(
        perfect_but_broken.score(Weights::default()) > weak_but_correct.score(Weights::default()),
        "the broken candidate does win on raw score -- that is the point"
    );

    let candidates = [perfect_but_broken, weak_but_correct];
    let pick = select::select(&candidates, &req, &policy, Weights::default());
    assert_eq!(
        pick,
        Some(1),
        "and it still loses, because it never entered the scored set"
    );

    assert_eq!(
        select::explain(&candidates[0], &req, &policy),
        Err(select::Excluded::GateFailed)
    );
    assert!(select::explain(&candidates[1], &req, &policy).is_ok());
}

#[test]
fn a_candidate_with_no_provider_is_excluded_distinctly() {
    let req = required();
    let policy = Policy::default();
    let absent = cand(
        "absent",
        SemVerX::uniform(5, 0, 0, State::Stable),
        SCORE_ONE,
        Votes::new(10, 0, 0),
        None,
    );
    assert_eq!(
        select::explain(&absent, &req, &policy),
        Err(select::Excluded::NotPresent),
        "'known about but not loadable' is a different report from 'shape wrong'"
    );
    assert_eq!(
        select::select(&[absent], &req, &policy, Weights::default()),
        None
    );
}

#[test]
fn selection_finding_nothing_is_reported_as_nothing() {
    let req = required();
    let policy = Policy::default();
    let all_broken = [
        cand(
            "b1",
            SemVerX::uniform(2, 0, 0, State::Stable),
            SCORE_ONE,
            Votes::new(5, 0, 0),
            Some(broken(SemVerX::uniform(2, 0, 0, State::Stable))),
        ),
        cand(
            "b2",
            SemVerX::uniform(3, 0, 0, State::Stable),
            SCORE_ONE,
            Votes::new(5, 0, 0),
            Some(broken(SemVerX::uniform(3, 0, 0, State::Stable))),
        ),
    ];
    assert_eq!(
        select::select(&all_broken, &req, &policy, Weights::default()),
        None,
        "no candidate can legally fill this slot -- not 'the best one scored low'"
    );
}

#[test]
fn the_policy_mask_also_filters_before_scoring() {
    let req = required();
    let stable_only = Policy::default();

    // Correct shape, but published as beta.
    let beta_version = SemVerX::new(1, State::Stable, 2, State::Beta, 0, State::Stable);
    let c = cand(
        "beta",
        beta_version,
        SCORE_ONE,
        Votes::new(9, 0, 0),
        Some(ExportDesc::provide(
            "add",
            &SIG_II_I,
            beta_version,
            add_i32 as *const () as *mut _,
        )),
    );

    assert_eq!(
        select::select(&[c], &req, &stable_only, Weights::default()),
        None,
        "a stable-only deployment does not get a beta build, however well it scores"
    );

    let testing = Policy::new(
        mmuko_kernel::NATIVE_ARCH,
        mmuko_kernel::semverx::StateMask::TESTING,
    );
    assert_eq!(
        select::select(&[c], &req, &testing, Weights::default()),
        Some(0),
        "and does get it once someone writes down that beta is acceptable"
    );
}

#[test]
fn ties_break_deterministically() {
    let req = required();
    let policy = Policy::default();
    let v_low = SemVerX::uniform(1, 0, 0, State::Stable);
    let v_high = SemVerX::uniform(1, 0, 5, State::Stable);

    // Identical on every scored axis except version: higher version wins.
    let a = cand("a", v_low, 500_000, Votes::new(1, 1, 0), Some(good(v_low)));
    let b = cand("b", v_high, 500_000, Votes::new(1, 1, 0), Some(good(v_high)));
    assert_eq!(select::select(&[a, b], &req, &policy, Weights::default()), Some(1));
    assert_eq!(
        select::select(&[b, a], &req, &policy, Weights::default()),
        Some(0),
        "and the answer does not depend on enumeration order"
    );

    // Identical on EVERY axis including version: the id breaks the tie, so the
    // choice is total and two machines cannot disagree.
    let x = cand("aaa", v_low, 500_000, Votes::new(1, 1, 0), Some(good(v_low)));
    let y = cand("zzz", v_low, 500_000, Votes::new(1, 1, 0), Some(good(v_low)));
    assert_eq!(select::select(&[x, y], &req, &policy, Weights::default()), Some(1));
    assert_eq!(select::select(&[y, x], &req, &policy, Weights::default()), Some(0));
}

// --------------------------------------------------------------------------
// Graph traversal
// --------------------------------------------------------------------------

#[test]
fn best_path_walks_the_highest_scoring_gated_branch() {
    // The article's demo graph: A -> {B1, B2}, B1 -> C1, B2 -> C2.
    let req = required();
    let policy = Policy::new(
        mmuko_kernel::NATIVE_ARCH,
        mmuko_kernel::semverx::StateMask::ANY,
    );
    let w = Weights::default();

    let mut g: SelectGraph<8> = SelectGraph::new();

    let va = parse_compact("1.0.0-stable").unwrap();
    let vb1 = parse_compact("2.0.0-stable").unwrap();
    let vb2 = parse_compact("1.5.2-experimental").unwrap();
    let vc1 = parse_compact("3.0.0-stable").unwrap();
    let vc2 = parse_compact("2.9.9-legacy").unwrap();

    // Every candidate keeps the int32 `add` contract, so the gate lets them
    // all through and the choice is purely about score.
    let a = g.add(cand("A", va, 950_000, Votes::new(2, 0, 1), Some(good(va)))).unwrap();
    let b1 = g.add(cand("B1", vb1, 900_000, Votes::new(3, 1, 0), Some(good(vb1)))).unwrap();
    let b2 = g.add(cand("B2", vb2, 700_000, Votes::new(1, 2, 1), Some(good(vb2)))).unwrap();
    let c1 = g.add(cand("C1", vc1, 920_000, Votes::new(2, 0, 0), Some(good(vc1)))).unwrap();
    let c2 = g.add(cand("C2", vc2, 850_000, Votes::new(1, 0, 1), Some(good(vc2)))).unwrap();

    g.edge(a, b1);
    g.edge(a, b2);
    g.edge(b1, c1);
    g.edge(b2, c2);

    let mut path = [0usize; 8];
    let n = g.best_path(a, &req, &policy, w, &mut path);
    let ids: Vec<&str> = path[..n].iter().map(|&i| g.get(i).unwrap().id).collect();
    assert_eq!(ids, vec!["A", "B1", "C1"], "the stable, well-endorsed branch wins");
}

#[test]
fn a_broken_branch_is_not_taken_however_well_it_scores() {
    let req = required();
    let policy = Policy::new(
        mmuko_kernel::NATIVE_ARCH,
        mmuko_kernel::semverx::StateMask::ANY,
    );
    let w = Weights::default();

    let mut g: SelectGraph<8> = SelectGraph::new();
    let va = parse_compact("1.0.0-stable").unwrap();
    let vb1 = parse_compact("2.0.0-stable").unwrap();
    let vb2 = parse_compact("1.5.2-experimental").unwrap();

    let a = g.add(cand("A", va, 950_000, Votes::new(2, 0, 1), Some(good(va)))).unwrap();
    // B1 scores best but changed `add` to take doubles.
    let b1 = g.add(cand("B1", vb1, 900_000, Votes::new(3, 1, 0), Some(broken(vb1)))).unwrap();
    let b2 = g.add(cand("B2", vb2, 700_000, Votes::new(1, 2, 1), Some(good(vb2)))).unwrap();
    g.edge(a, b1);
    g.edge(a, b2);

    let mut path = [0usize; 8];
    let n = g.best_path(a, &req, &policy, w, &mut path);
    let ids: Vec<&str> = path[..n].iter().map(|&i| g.get(i).unwrap().id).collect();
    assert_eq!(
        ids,
        vec!["A", "B2"],
        "the resolver takes the worse-scoring branch that actually works"
    );
}

#[test]
fn a_cycle_terminates_the_walk_rather_than_hanging() {
    let req = required();
    let policy = Policy::default();
    let v = SemVerX::uniform(1, 0, 0, State::Stable);

    let mut g: SelectGraph<4> = SelectGraph::new();
    let a = g.add(cand("A", v, SCORE_ONE, Votes::new(1, 0, 0), Some(good(v)))).unwrap();
    let b = g.add(cand("B", v, SCORE_ONE, Votes::new(1, 0, 0), Some(good(v)))).unwrap();
    g.edge(a, b);
    g.edge(b, a);

    let mut path = [0usize; 4];
    let n = g.best_path(a, &req, &policy, Weights::default(), &mut path);
    assert_eq!(n, 2, "a cycle stops the walk; a hanging resolver is worse than a short path");
}

#[test]
fn graph_capacity_is_enforced() {
    let v = SemVerX::uniform(1, 0, 0, State::Stable);
    let mut g: SelectGraph<2> = SelectGraph::new();
    assert!(g.add(cand("a", v, 0, Votes::none(), None)).is_some());
    assert!(g.add(cand("b", v, 0, Votes::none(), None)).is_some());
    assert!(g.add(cand("c", v, 0, Votes::none(), None)).is_none());
    assert_eq!(g.find("b"), Some(1));
    assert_eq!(g.find("c"), None);
    assert!(!g.edge(0, 9), "an edge to a nonexistent node is refused");
}

// --------------------------------------------------------------------------
// The compact input dialect
// --------------------------------------------------------------------------

#[test]
fn compact_versions_parse_into_semverx() {
    let v = parse_compact("1.5.2-experimental").unwrap();
    assert_eq!(v.major, 1);
    assert_eq!(v.minor, 5);
    assert_eq!(v.patch, 2);
    assert_eq!(v.major_state, State::Experimental);
    assert_eq!(
        v.patch_state,
        State::Experimental,
        "the single channel applies to all three fields"
    );

    assert_eq!(parse_compact("2.0.0-lts").unwrap().major_state, State::Lts);
    assert_eq!(
        parse_compact("2.9.9-legacy").unwrap().major_state,
        State::Legacy
    );
}

#[test]
fn a_missing_channel_is_an_error_not_a_default() {
    // The one place this dialect differs from an ordinary SemVer parser, and
    // the reason it can be trusted: a guessed state would be indistinguishable
    // downstream from a state somebody actually declared.
    assert_eq!(parse_compact("1.2.3"), None);
    assert_eq!(parse_compact("1.2.3-"), None);
    assert_eq!(parse_compact("1.2.3-golden"), None);
    assert_eq!(parse_compact("1.2-stable"), None);
    assert_eq!(parse_compact("1.2.3.4-stable"), None);
    assert_eq!(parse_compact(""), None);
}

#[test]
fn the_compact_form_and_the_canonical_form_agree() {
    let compact = parse_compact("4.17.2-stable").unwrap();
    let canonical = SemVerX::parse("4.stable.17.stable.2.stable").unwrap();
    assert_eq!(compact, canonical);
    assert!(
        SemVerX::parse("4.17.2-stable").is_none(),
        "the canonical reader does not accept the dialect; they stay distinct"
    );
}
