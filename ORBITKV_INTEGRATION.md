# OrbitKV TENT integration

The `orbitkv-integration` branch is the native dependency used by
[OrbitInfra/OrbitKV](https://github.com/OrbitInfra/OrbitKV). OrbitKV pins a specific
commit through its submodule and builds `tent_shared`; it does not fetch the
moving branch head during a build or publish a separate Mooncake Python package.

The upstream base is `v0.3.13.post1`, commit
`719735896c86b56fabec6cf3e825fb2ea640597a`. The initial six commits preserve the
exact native source previously produced by OrbitKV's build patches:

| Commit | Consumed behavior and origin |
| --- | --- |
| `e4133437` | TCP copy status and CUDA context/retention; scoped upstream #3476/#3973 backports plus existing local ownership fixes |
| `784e974f` | TCP worker terminal ownership; scoped upstream #4001 backport |
| `84a385c3` | Whole-source TCP admission and dispatch drain |
| `8e675714` | Source-grant epoch and matching TCP peer wire/ABI |
| `f877eeb2` | Unregister retry and failed MR ownership; adaptation of upstream #4573/#4249 |
| `a4d25a1b` | Partial registration rollback ownership; continuation of that adaptation |

Upstream #3476, #3973 and #4001 are merged; #4573 and #4249 remained open at
the 2026-10-21 check. These are selected adaptations, not full cherry-picks.
The [original contract and qualification notes](https://github.com/OrbitInfra/OrbitKV/blob/4cd00b809b4ee81de89e4712fadda8208459840f/crates/orbitkv-mooncake-sys/patches/README.md)
retain the detailed provenance and limitations. CPU source drain does not prove
CUDA/RDMA physical termination; failed native ownership remains retained.

Keep this branch separate from upstream `main`. Incorporate upstream fixes as
reviewable commits and retire superseded local changes after their replacements
pass the consuming OrbitKV gates. Do not force-push commits already pinned by
OrbitKV. Changes to wire/ABI, registration, copy or completion ownership require
their relevant native and installed-runtime checks before updating the submodule.
