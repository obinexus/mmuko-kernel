// SPDX-License-Identifier: LicenseRef-OBINexus-1.0
//
//! SemVerX: `major.state.minor.state.patch.state`.
//!
//! Classic SemVer says `4.17.2`. SemVerX says `4.stable.17.beta.2.stable`, and
//! the `.state` field is part of the IDENTITY rather than a label attached to
//! it. `4.17.15-stable` and `4.17.15-legacy` are different versions to the
//! resolver, which is what stops a cached legacy build masquerading as the
//! stable one that CI actually tested.
//!
//! `no_std`. No allocation, no locale, no floating point.

use crate::fingerprint::{Canon, EncodeError};

/// Maturity state.
///
/// Ordered by maturity, but this is NOT a compatibility ordering. It exists so
/// a policy can express "stable only" or "beta and above" as a mask test. Two
/// versions with different states are never the same version, however their
/// numbers compare.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
#[repr(u8)]
pub enum State {
    /// Not a valid state. Present so a zeroed struct is detectably invalid
    /// rather than silently "experimental".
    Invalid = 0,
    /// Unreviewed. Shape may move without notice.
    Experimental = 1,
    /// Feature complete, under test.
    Beta = 2,
    /// Supported. The default deployment target.
    Stable = 3,
    /// Superseded but still published.
    Legacy = 4,
    /// Long-term support: a stable line held open past the point where the
    /// mainline moved on. Distinct from [`State::Stable`] because a caller may
    /// want one and not the other, and identical to it in every other respect.
    Lts = 5,
}

impl State {
    /// The token used in the canonical encoding and in parsing.
    pub const fn name(self) -> &'static str {
        match self {
            State::Experimental => "experimental",
            State::Beta => "beta",
            State::Stable => "stable",
            State::Legacy => "legacy",
            State::Lts => "lts",
            State::Invalid => "invalid",
        }
    }

    /// Parse a state token. `None` for anything unrecognised -- never a
    /// default, because a guessed state is the ambiguity SemVerX removes.
    pub fn parse(tok: &str) -> Option<State> {
        Some(match tok {
            "experimental" => State::Experimental,
            "beta" => State::Beta,
            "stable" => State::Stable,
            "legacy" => State::Legacy,
            "lts" => State::Lts,
            _ => return None,
        })
    }

    /// Convert from the raw `u8` a C table carries.
    pub const fn from_raw(v: u8) -> State {
        match v {
            1 => State::Experimental,
            2 => State::Beta,
            3 => State::Stable,
            4 => State::Legacy,
            5 => State::Lts,
            _ => State::Invalid,
        }
    }

    /// This state's bit in a [`StateMask`].
    pub const fn bit(self) -> u32 {
        1u32 << (self as u32)
    }
}

/// A set of acceptable states.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StateMask(pub u32);

impl StateMask {
    /// Stable and LTS. The default: opting in to risk should be explicit.
    pub const STABLE: StateMask = StateMask(State::Stable.bit() | State::Lts.bit());
    /// Stable, LTS and beta.
    pub const TESTING: StateMask = StateMask(Self::STABLE.0 | State::Beta.bit());
    /// Stable, LTS, beta and experimental.
    pub const DEV: StateMask = StateMask(Self::TESTING.0 | State::Experimental.bit());
    /// Every state, legacy included.
    pub const ANY: StateMask = StateMask(Self::DEV.0 | State::Legacy.bit());

    /// Whether `s` is admitted by this mask.
    pub const fn admits(self, s: State) -> bool {
        (self.0 & s.bit()) != 0
    }
}

/// A SemVerX version.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SemVerX {
    /// Major number.
    pub major: u16,
    /// Minor number.
    pub minor: u16,
    /// Patch number.
    pub patch: u16,
    /// State of the major field.
    pub major_state: State,
    /// State of the minor field.
    pub minor_state: State,
    /// State of the patch field.
    pub patch_state: State,
}

/// Why a version did not satisfy a requirement.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Unsatisfied {
    /// One of the versions was structurally invalid.
    Malformed,
    /// Different major line.
    Major,
    /// A state was outside the policy mask, or the major-state identity
    /// differed.
    StateRefused,
    /// The provider is older than required within its line.
    TooOld,
}

impl SemVerX {
    /// A version with the same state on every field. The common case.
    pub const fn uniform(major: u16, minor: u16, patch: u16, state: State) -> SemVerX {
        SemVerX {
            major,
            minor,
            patch,
            major_state: state,
            minor_state: state,
            patch_state: state,
        }
    }

    /// A version with an independent state per field.
    pub const fn new(
        major: u16,
        major_state: State,
        minor: u16,
        minor_state: State,
        patch: u16,
        patch_state: State,
    ) -> SemVerX {
        SemVerX {
            major,
            minor,
            patch,
            major_state,
            minor_state,
            patch_state,
        }
    }

    /// Structural validity: every state field in range.
    pub const fn is_valid(&self) -> bool {
        !matches!(self.major_state, State::Invalid)
            && !matches!(self.minor_state, State::Invalid)
            && !matches!(self.patch_state, State::Invalid)
    }

    /// Parse `major.state.minor.state.patch.state`.
    ///
    /// All six fields are required. A shorter form is rejected rather than
    /// defaulted: an omitted state is precisely the ambiguity SemVerX exists
    /// to remove, so supplying one at the parser would reintroduce it at the
    /// cheapest possible place to catch it.
    pub fn parse(s: &str) -> Option<SemVerX> {
        let mut it = s.split('.');
        let major = it.next()?.parse::<u16>().ok()?;
        let major_state = State::parse(it.next()?)?;
        let minor = it.next()?.parse::<u16>().ok()?;
        let minor_state = State::parse(it.next()?)?;
        let patch = it.next()?.parse::<u16>().ok()?;
        let patch_state = State::parse(it.next()?)?;
        if it.next().is_some() {
            return None; // trailing junk
        }
        Some(SemVerX {
            major,
            minor,
            patch,
            major_state,
            minor_state,
            patch_state,
        })
    }

    /// Exact identity across all six fields.
    pub fn identical(&self, other: &SemVerX) -> bool {
        self == other
    }

    /// Ordering on the numbers only. States are ignored here because they do
    /// not form a compatibility order; use [`SemVerX::satisfies`] for that.
    pub fn cmp_numeric(&self, other: &SemVerX) -> core::cmp::Ordering {
        (self.major, self.minor, self.patch).cmp(&(other.major, other.minor, other.patch))
    }

    /// Does `provided` satisfy a call site requiring `self`, under `mask`?
    ///
    /// 1. Both versions must be structurally valid.
    /// 2. Every state field of the provider must be in `mask`.
    /// 3. `major` and `major_state` must be identical -- the major state is
    ///    part of the identity of the line.
    /// 4. `(minor, patch)` of the provider must be at least the required.
    ///
    /// This is never sufficient for binding. Version satisfaction is a policy
    /// question; ABI identity is a machine question, and
    /// [`crate::trident`] decides the machine question first and
    /// independently. A module may bump only its patch number and still have
    /// changed a parameter width, and no version rule can rescue that.
    pub fn satisfies(&self, provided: &SemVerX, mask: StateMask) -> Result<(), Unsatisfied> {
        if !self.is_valid() || !provided.is_valid() {
            return Err(Unsatisfied::Malformed);
        }
        // Every field, not just the major. A package whose patch line is beta
        // is a beta package however stable its major line claims to be.
        if !mask.admits(provided.major_state)
            || !mask.admits(provided.minor_state)
            || !mask.admits(provided.patch_state)
        {
            return Err(Unsatisfied::StateRefused);
        }
        if self.major != provided.major {
            return Err(Unsatisfied::Major);
        }
        if self.major_state != provided.major_state {
            return Err(Unsatisfied::StateRefused);
        }
        if provided.minor < self.minor
            || (provided.minor == self.minor && provided.patch < self.patch)
        {
            return Err(Unsatisfied::TooOld);
        }
        Ok(())
    }

    /// Append the canonical rendering to `out`.
    pub fn encode(&self, out: &mut Canon) -> Result<(), EncodeError> {
        if !self.is_valid() {
            return Err(EncodeError::MalformedType);
        }
        out.put_u64(self.major as u64);
        out.put_u8(b'.');
        out.put(self.major_state.name());
        out.put_u8(b'.');
        out.put_u64(self.minor as u64);
        out.put_u8(b'.');
        out.put(self.minor_state.name());
        out.put_u8(b'.');
        out.put_u64(self.patch as u64);
        out.put_u8(b'.');
        out.put(self.patch_state.name());
        if out.overflowed() {
            Err(EncodeError::Overflow)
        } else {
            Ok(())
        }
    }

    /// Convert from the raw C struct, mapping unknown states to
    /// [`State::Invalid`] rather than trusting them.
    pub const fn from_raw(r: &crate::abi::raw::CSemVerX) -> SemVerX {
        SemVerX {
            major: r.major,
            minor: r.minor,
            patch: r.patch,
            major_state: State::from_raw(r.major_state),
            minor_state: State::from_raw(r.minor_state),
            patch_state: State::from_raw(r.patch_state),
        }
    }
}
