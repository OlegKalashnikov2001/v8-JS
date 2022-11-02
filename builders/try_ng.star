# Copyright 2020 the V8 project authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

load(
    "//lib/lib.star",
    "CQ",
    "GCLIENT_VARS",
    "GOMA",
    "defaults_triggered",
    "defaults_try",
    "v8_builder",
)

# Orchestrator migration states.
ORCHESTRATOR = struct(
    OPTIONAL = 0,
    EXP_20_PERCENT = 1,
    EXP_100_PERCENT = 2,
    MIGRATED = 3,
)

#TODO(almuthanna): get rid of kwargs and specify default values
def try_ng_pair(
        name,
        cq_properties = CQ.NONE,
        cq_branch_properties = CQ.NONE,
        cq_compile_only_properties = CQ.OPTIONAL,
        cq_branch_compile_only_properties = CQ.OPTIONAL,
        experiments = None,
        enable_rdb = True,
        orchestrator = ORCHESTRATOR.OPTIONAL,
        **kwargs):
    triggered_timeout = kwargs.pop("triggered_timeout", None)

    # All unspecified branch trybots are per default optional.
    if (cq_properties != CQ.NONE and cq_branch_properties == CQ.NONE):
        cq_branch_properties = CQ.OPTIONAL

    description = kwargs.pop("description", None)
    compiler_description, tester_description = None, None
    if description:
        compiler_description = dict(description)
        tester_description = dict(description)
        compiler_description["triggers"] = name + "_ng_triggered"
        tester_description["triggered by"] = name + "_ng"

    kwargs["enable_rdb"] = enable_rdb

    # Generate orchestrator trybot.
    # The corresponding compilator name gets an infix like:
    # v8_linux_rel -> v8_linux_compile_rel.
    prefix, suffix = name.rsplit("_", 1)
    compilator_name = prefix + "_compile_" + suffix
    v8_builder(
        defaults_triggered,
        name = name,
        bucket = "try",
        execution_timeout = triggered_timeout,
        executable = "recipe:v8/orchestrator",
        cq_properties = cq_properties,
        cq_branch_properties = cq_branch_properties,
        in_list = "tryserver",
        experiments = experiments,
        description = tester_description,
        properties = {"compilator_name": compilator_name},
    )

    # Generate compilator trybot.
    v8_builder(
        defaults_try,
        name = compilator_name,
        bucket = "try",
        executable = "recipe:v8/compilator",
        cq_properties = cq_compile_only_properties,
        cq_branch_properties = cq_branch_compile_only_properties,
        in_list = "tryserver",
        experiments = experiments,
        description = compiler_description,
        **kwargs
    )

try_ng_pair(
    name = "v8_android_arm64_n5x_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    properties = {"target_platform": "android", "target_arch": "arm"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_fuchsia_rel",
    cq_properties = CQ.EXP_100_PERCENT,
    cq_branch_properties = CQ.EXP_100_PERCENT,
    cq_compile_only_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    properties = {"target_platform": "fuchsia"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_arm64_no_pointer_compression_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_asan_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_cfi_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_cppgc_non_default_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_no_sandbox_dbg",
    cq_properties = CQ.OPTIONAL,
    cq_branch_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_dbg",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_minor_mc_dbg",
    cq_properties = CQ.on_files(
        ".+/[+]/test/cctest/heap/.+",
        ".+/[+]/test/unittests/heap/.+",
    ),
    cq_branch_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_dict_tracking_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_disable_runtime_call_stats_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_external_code_space_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_gc_stress_custom_snapshot_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_gc_stress_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_heap_sandbox_dbg",
    cq_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_fuzzilli_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_fyi_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_gcc_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-20.04", "cpu": "x86-64"},
    execution_timeout = 5400,
    use_goma = GOMA.NO,
)

try_ng_pair(
    name = "v8_linux64_msan_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    properties = {"gclient_vars": {"checkout_instrumented_libraries": "True"}},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_nodcheck_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_perfetto_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_no_pointer_compression_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_no_sandbox_rel",
    cq_properties = CQ.OPTIONAL,
    cq_branch_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_single_generation_dbg",
    cq_properties = CQ.on_files(
        ".+/[+]/test/cctest/heap/.+",
        ".+/[+]/test/unittests/heap/.+",
    ),
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
    orchestrator = ORCHESTRATOR.MIGRATED,
)

try_ng_pair(
    name = "v8_linux64_predictable_rel",
    cq_properties = CQ.OPTIONAL,
    cq_branch_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_riscv32_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_riscv64_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_loong64_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_tsan_rel",
    cq_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_tsan_no_cm_rel",
    cq_properties = CQ.on_files(
        ".+/[+]/src/compiler/js-heap-broker.(h|cc)",
        ".+/[+]/src/compiler/heap-refs.(h|cc)",
    ),
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_tsan_isolates_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_ubsan_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux64_verify_csa_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm64_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm64_gc_stress_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm64_sim_heap_sandbox_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
    description = {
        "purpose": "Arm64 simulator heap sandbox",
        "request": "https://crbug.com/v8/12257",
        "ci_base": "V8 Linux64 - arm64 - sim - heap sandbox - debug",
    },
)

try_ng_pair(
    name = "v8_linux_arm64_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm64_cfi_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm_lite_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_arm_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 2400,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_gc_stress_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_nodcheck_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 2400,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_noi18n_rel",
    cq_properties = CQ.on_files(".+/[+]/.*intl.*", ".+/[+]/.*test262.*"),
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 2400,
    gclient_vars = [GCLIENT_VARS.GCMOLE],
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_optional_rel",
    cq_properties = CQ.on_files(
        ".+/[+]/src/codegen/shared-ia32-x64/macro-assembler-shared-ia32-x64.(h|cc)",
        ".+/[+]/src/codegen/x64/(macro-)?assembler-x64.(h|cc)",
        ".+/[+]/src/codegen/x64/sse-instr.h",
        ".+/[+]/src/compiler/backend/x64/code-generator-x64.cc",
        ".+/[+]/src/wasm/baseline/x64/liftoff-assembler-x64.h",
    ),
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_linux_verify_csa_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    execution_timeout = 2400,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac64_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac64_asan_rel",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    # TODO(almuthanna): add this to Branch CQ after current milestone + 3
    # (i.e. M100).
    cq_compile_only_properties = CQ.EXP_100_PERCENT,
    dimensions = {"os": "Mac-10.15"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac64_gc_stress_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    execution_timeout = 3600,
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac64_rel",
    triggered_timeout = 7200,
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
    orchestrator = ORCHESTRATOR.MIGRATED,
)

try_ng_pair(
    name = "v8_mac_arm64_rel",
    triggered_timeout = 7200,
    cq_properties = CQ.BLOCK,
    # TODO(https://crbug.com/v8/13008): Promote to blocking after M110.
    cq_branch_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_full_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_no_pointer_compression_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_sim_rel",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_sim_dbg",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_mac_arm64_sim_nodcheck_rel",
    triggered_timeout = 7200,
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Mac-10.15"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_numfuzz_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_numfuzz_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_numfuzz_tsan_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_odroid_arm_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Ubuntu-18.04", "cpu": "x86-64"},
    properties = {"target_arch": "arm"},
    use_goma = GOMA.DEFAULT,
)

try_ng_pair(
    name = "v8_win64_dbg",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.ATS,
)

try_ng_pair(
    name = "v8_win64_msvc_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    execution_timeout = 3600,
    properties = {"use_goma": False},
    use_goma = GOMA.NO,
)

try_ng_pair(
    name = "v8_win64_rel",
    cq_properties = CQ.BLOCK,
    cq_branch_properties = CQ.BLOCK,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    triggered_timeout = 7200,
    use_goma = GOMA.ATS,
    orchestrator = ORCHESTRATOR.MIGRATED,
)

try_ng_pair(
    name = "v8_win_dbg",
    cq_properties = CQ.OPTIONAL,
    cq_compile_only_properties = CQ.BLOCK,
    cq_branch_compile_only_properties = CQ.BLOCK,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.ATS,
)

try_ng_pair(
    name = "v8_win_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    use_goma = GOMA.ATS,
)

try_ng_pair(
    name = "v8_win64_asan_rel",
    cq_properties = CQ.OPTIONAL,
    dimensions = {"os": "Windows-10", "cpu": "x86-64"},
    execution_timeout = 3600,
    use_goma = GOMA.ATS,
)
