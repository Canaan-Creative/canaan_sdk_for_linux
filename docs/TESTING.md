# Host tests

The host tests need no board and no image build.

| Script | Runs |
|---|---|
| `test/scripts/host_tests.sh` | everything below, plus the host CTest suites of `apps/miner` and `apps/miner-web`, and builds of `mc_mgmt` and `core_miner` that check the embedded public keys, the `libcrypto` link, the default protocol version and the API plugin stub |
| `test/scripts/conf_check.sh` | source-side checks of the products' and `meta-canaan`'s build and image configuration |
| `test/scripts/shellcheck.sh` | ShellCheck at `-S warning`, excluding SC2034, SC1090 and SC2046, on every tracked shell script; needs a git checkout |

```sh
test/scripts/host_tests.sh
docker/build.sh --run test/scripts/host_tests.sh   # in the build container
```

## Requirements

`shellcheck`, `cmake`, a C compiler, `readelf`, `python3` with `cryptography`, and OpenSSL headers:
`OPENSSL_ROOT_DIR`, else the `openssl-native` sysroot a prior `./build.sh <product>` stages under
`build/`, else the system `libssl-dev`. The build container has them all.

| Variable / tool | Effect |
|---|---|
| `HOST_TESTS_JOBS` | parallel build and CTest jobs, default `nproc` |
| `LVGL_SOURCE_DIR` | local checkout of the LVGL the `lvgl` recipe builds; without it the `miner_mgmt` suite downloads LVGL on every run |
| `busybox` on the `PATH` | also runs the `miner_web` init-script test under `busybox sh`; without it only `init_busybox` is skipped |
| `node` on the `PATH` | runs the web page test; without it the test is skipped |

`docker/build.sh --run` passes only its own fixed variables into the container, so
`HOST_TESTS_JOBS`, `LVGL_SOURCE_DIR` and `OPENSSL_ROOT_DIR` apply to native runs only.

## Results

| Output | Where |
|---|---|
| TAP | stdout |
| JUnit XML | `host-tests.xml`, `conf-check.xml`, `shellcheck.xml` in `test/results/` |
| Per-suite log | `test/results/logs/host-<suite>.log`; a failure prints its last 20 lines |

In `host_tests.sh` each suite is one assertion. A CTest case that skips is reported as a TAP
`# SKIP` line and does not fail the run.
