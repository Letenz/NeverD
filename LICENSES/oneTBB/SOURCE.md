# oneTBB

Source: [NeverSight/oneTBB at 80dfa673b2d9df4edaa0d7ad2b6d28cc6ecae2e5](https://github.com/NeverSight/oneTBB/tree/80dfa673b2d9df4edaa0d7ad2b6d28cc6ecae2e5).
Based on upstream [uxlfoundation/oneTBB v2023.1.0](https://github.com/uxlfoundation/oneTBB/tree/v2023.1.0).

License: Apache-2.0. The original copyright notices and license texts are
retained here and in the downloaded source.

Fork changes (NeverSight, 2026-10-19): select explicit narrow Windows dynamic
loader APIs so that Unicode-enabled embedding builds compile; enable Unicode
macros for the dynamic loader regression and keep its helper's narrow API.
NeverD applies no additional local source patches.
`cmake/NeverDPerformance.cmake` pins both the fork commit and archive SHA-256.
