# Abseil with Zombie Linear Probing

## Quickstart

```bash
mkdir build
cd build
cmake ../ -DCMAKE_BUILD_TYPE=Debug  -DZOMBIE_LINEAR_PROBING=ON -DABSL_GOOGLETEST_DOWNLOAD_URL=https://github.com/google/googletest/archive/2dd1c131950043a8ad5ab0d2dda0e0970596586a.zip -DABSL_BUILD_TESTING=ON
make -j
./bin/absl_raw_hash_set_test
```

### To run probe benchmark

(Not sure what the probe benchmark does yet.)

```bash
# Get Bazel, add below binary to $PATH as bazel
wget https://github.com/bazelbuild/bazelisk/releases/download/v1.19.0/bazelisk-linux-amd64

bazel run raw_hash_set_probe_benchmark
bazel run raw_hash_set_probe_benchmark_zlp
```

## Notes

```c++
// absl/container/flat_hash_map.h
flat_hash_map<K, V, Hash, Eq, Allocator>: 
	raw_hash_map<FlatHashMapPolicy<K,V,Hash,Eq,Allocator>


// absl/container/internal/raw_hash_map.h
class raw_hash_map : public raw_hash_set<Policy, Hash, Eq, Alloc> 

// absl/container/internal/raw_hash_set.h
// An open-addressing
// hashtable with quadratic probing.
// IMPLEMENTATION DETAILS
//
// # Table Layout
//
// A raw_hash_set's backing array consists of control bytes followed by slots
// that may or may not contain objects.
//
// The layout of the backing array, for `capacity` slots, is thus, as a
// pseudo-struct:
//
//   struct BackingArray {
//     // Sampling handler. This field isn't present when the sampling is
//     // disabled or this allocation hasn't been selected for sampling.
//     HashtablezInfoHandle infoz_;
//     // The number of elements we can insert before growing the capacity.
//     size_t growth_left;
//     // Control bytes for the "real" slots.
//     ctrl_t ctrl[capacity];
//     // Always `ctrl_t::kSentinel`. This is used by iterators to find when to
//     // stop and serves no other purpose.
//     ctrl_t sentinel;
//     // A copy of the first `kWidth - 1` elements of `ctrl`. This is used so
//     // that if a probe sequence picks a value near the end of `ctrl`,
//     // `Group` will have valid control bytes to look at.
//     ctrl_t clones[kWidth - 1];
//     // The actual slot data.
//     slot_type slots[capacity];
//   };
//

//raw_hash_set:467
// The values here are selected for maximum performance. See the static asserts
// below for details.

// A `ctrl_t` is a single control byte, which can have one of four
// states: empty, deleted, full (which has an associated seven-bit h2_t value)
// and the sentinel. They have the following bit patterns:
//
//      empty: 1 0 0 0 0 0 0 0
//    deleted: 1 1 1 1 1 1 1 0
//       full: 0 h h h h h h h  // h represents the hash bits.
//   sentinel: 1 1 1 1 1 1 1 1
//
// These values are specifically tuned for SSE-flavored SIMD.
// The static_asserts below detail the source of these choices.
//
// We use an enum class so that when strict aliasing is enabled, the compiler
// knows ctrl_t doesn't alias other types.


// Vectorized Instruction implementation
// raw_hash_set:617
struct GroupSse2Impl {
}


// Insert Code
// First find
```



# Abseil - C++ Common Libraries

The repository contains the Abseil C++ library code. Abseil is an open-source
collection of C++ code (compliant to C++14) designed to augment the C++
standard library.

## Table of Contents

- [About Abseil](#about)
- [Quickstart](#quickstart)
- [Building Abseil](#build)
- [Support](#support)
- [Codemap](#codemap)
- [Releases](#releases)
- [License](#license)
- [Links](#links)

<a name="about"></a>
## About Abseil

Abseil is an open-source collection of C++ library code designed to augment
the C++ standard library. The Abseil library code is collected from Google's
own C++ code base, has been extensively tested and used in production, and
is the same code we depend on in our daily coding lives.

In some cases, Abseil provides pieces missing from the C++ standard; in
others, Abseil provides alternatives to the standard for special needs
we've found through usage in the Google code base. We denote those cases
clearly within the library code we provide you.

Abseil is not meant to be a competitor to the standard library; we've
just found that many of these utilities serve a purpose within our code
base, and we now want to provide those resources to the C++ community as
a whole.

<a name="quickstart"></a>
## Quickstart

If you want to just get started, make sure you at least run through the
[Abseil Quickstart](https://abseil.io/docs/cpp/quickstart). The Quickstart
contains information about setting up your development environment, downloading
the Abseil code, running tests, and getting a simple binary working.

<a name="build"></a>
## Building Abseil

[Bazel](https://bazel.build) and [CMake](https://cmake.org/) are the official
build systems for Abseil.
See the [quickstart](https://abseil.io/docs/cpp/quickstart) for more information
on building Abseil using the Bazel build system.
If you require CMake support, please check the [CMake build
instructions](CMake/README.md) and [CMake
Quickstart](https://abseil.io/docs/cpp/quickstart-cmake).

<a name="support"></a>
## Support

Abseil follows Google's [Foundational C++ Support
Policy](https://opensource.google/documentation/policies/cplusplus-support). See
[this
table](https://github.com/google/oss-policies-info/blob/main/foundational-cxx-support-matrix.md)
for a list of currently supported versions compilers, platforms, and build
tools.

<a name="codemap"></a>
## Codemap

Abseil contains the following C++ library components:

* [`base`](absl/base/)
  <br /> The `base` library contains initialization code and other code which
  all other Abseil code depends on. Code within `base` may not depend on any
  other code (other than the C++ standard library).
* [`algorithm`](absl/algorithm/)
  <br /> The `algorithm` library contains additions to the C++ `<algorithm>`
  library and container-based versions of such algorithms.
* [`cleanup`](absl/cleanup/)
  <br /> The `cleanup` library contains the control-flow-construct-like type
  `absl::Cleanup` which is used for executing a callback on scope exit.
* [`container`](absl/container/)
  <br /> The `container` library contains additional STL-style containers,
  including Abseil's unordered "Swiss table" containers.
* [`crc`](absl/crc/) The `crc` library contains code for
  computing error-detecting cyclic redundancy checks on data.
* [`debugging`](absl/debugging/)
  <br /> The `debugging` library contains code useful for enabling leak
  checks, and stacktrace and symbolization utilities.
* [`flags`](absl/flags/)
  <br /> The `flags` library contains code for handling command line flags for
  libraries and binaries built with Abseil.
* [`hash`](absl/hash/)
  <br /> The `hash` library contains the hashing framework and default hash
  functor implementations for hashable types in Abseil.
* [`log`](absl/log/)
  <br /> The `log` library contains `LOG` and `CHECK` macros and facilities
  for writing logged messages out to disk, `stderr`, or user-extensible
  destinations.
* [`memory`](absl/memory/)
  <br /> The `memory` library contains memory management facilities that augment
  C++'s `<memory>` library.
* [`meta`](absl/meta/)
  <br /> The `meta` library contains compatible versions of type checks
  available within C++14 and C++17 versions of the C++ `<type_traits>` library.
* [`numeric`](absl/numeric/)
  <br /> The `numeric` library contains 128-bit integer types as well as
  implementations of C++20's bitwise math functions.
* [`profiling`](absl/profiling/)
  <br /> The `profiling` library contains utility code for profiling C++
  entities.  It is currently a private dependency of other Abseil libraries.
* [`random`](absl/random/)
  <br /> The `random` library contains functions for generating psuedorandom
  values.
* [`status`](absl/status/)
  <br /> The `status` library contains abstractions for error handling,
  specifically `absl::Status` and `absl::StatusOr<T>`.
* [`strings`](absl/strings/)
  <br /> The `strings` library contains a variety of strings routines and
  utilities, including a C++14-compatible version of the C++17
  `std::string_view` type.
* [`synchronization`](absl/synchronization/)
  <br /> The `synchronization` library contains concurrency primitives (Abseil's
  `absl::Mutex` class, an alternative to `std::mutex`) and a variety of
  synchronization abstractions.
* [`time`](absl/time/)
  <br /> The `time` library contains abstractions for computing with absolute
  points in time, durations of time, and formatting and parsing time within
  time zones.
* [`types`](absl/types/)
  <br /> The `types` library contains non-container utility types, like a
  C++14-compatible version of the C++17 `std::optional` type.
* [`utility`](absl/utility/)
  <br /> The `utility` library contains utility and helper code.

<a name="releases"></a>
## Releases

Abseil recommends users "live-at-head" (update to the latest commit from the
master branch as often as possible). However, we realize this philosophy doesn't
work for every project, so we also provide [Long Term Support
Releases](https://github.com/abseil/abseil-cpp/releases) to which we backport
fixes for severe bugs. See our [release
management](https://abseil.io/about/releases) document for more details.

<a name="license"></a>
## License

The Abseil C++ library is licensed under the terms of the Apache
license. See [LICENSE](LICENSE) for more information.

<a name="links"></a>
## Links

For more information about Abseil:

* Consult our [Abseil Introduction](https://abseil.io/about/intro)
* Read [Why Adopt Abseil](https://abseil.io/about/philosophy) to understand our
  design philosophy.
* Peruse our
  [Abseil Compatibility Guarantees](https://abseil.io/about/compatibility) to
  understand both what we promise to you, and what we expect of you in return.
