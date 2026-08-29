package Logos::Generator::hookkit::Generator;
use strict;
use parent qw(Logos::Generator::Base::Generator);

sub findPreamble {
    # Always report "no preamble present" so Logos inserts ours.
    # The base check looks for logos/logos.h, which is always present in a
    # %hook file; if we reported present, our HookKit includes would never be
    # emitted (the static hook helper would then fail to compile).
    return 0;
}

sub preamble {
    my $self = shift;
    my $skipIncludes = shift;
    if ($skipIncludes) {
        return $self->SUPER::preamble();
    } else {
        return join("\n", ($self->SUPER::preamble(),
            "#include <HookKit/HookKit.h>",
            "#include <HookKit/HookKitObjC.h>",
            "#include <objc/runtime.h>",
            "#include <pthread.h>",
            "#include <string.h>",
        ));
    }
}

sub staticDeclarations {
    my $self = shift;
    return join("\n", ($self->SUPER::staticDeclarations(),
        "__asm__(\".linker_option \\\"-framework\\\", \\\"HookKit\\\"\");",
        "",
        "// HookKit runtime singleton for Logos %hook — lazy, early-process",
        "static hk_runtime_t *_hk_hookkit_rt = NULL;",
        "static pthread_once_t _hk_hookkit_once = PTHREAD_ONCE_INIT;",
        "static void _hk_hookkit_rt_init(void) {",
        "    hk_runtime_config_t _c; memset(&_c, 0, sizeof(_c));",
        "    _c.struct_size = sizeof(_c); _c.struct_version = HK_ABI_VERSION_3_0;",
        "    _c.install_context = HK_INSTALL_CONTEXT_EARLY_PROCESS;",
        "    (void)hk_runtime_create(&_c, &_hk_hookkit_rt);",
        "}",
        "static inline hk_runtime_t *_hk_hookkit_runtime(void) {",
        "    (void)pthread_once(&_hk_hookkit_once, _hk_hookkit_rt_init);",
        "    return _hk_hookkit_rt;",
        "}",
        "// Shared process singleton — prefer hk_shared_runtime() when available (cuts per-TU calloc+HKIDs).",
        "// Per-TU statics above remain for compat; this wrapper delegates to the framework global.",
        "__attribute__((unused)) static inline hk_runtime_t *_hk_hookkit_shared_runtime(void) {",
        "    extern hk_runtime_t *hk_shared_runtime(void);",
        "    hk_runtime_t *r = hk_shared_runtime();",
        "    return r ? r : _hk_hookkit_runtime();",
        "}",
        "__attribute__((unused)) static int _hk_hookkit_hook_message(const char *_stable_hook_id, Class _cls, SEL _sel, IMP _new, IMP *_old, int _isMeta) {",
        "    if (!_cls || !_sel || !_new) return -1;",
        "    hk_runtime_t *_rt = _hk_hookkit_shared_runtime(); if (!_rt) return -1;",
        "    hk_plan_t *_plan = NULL; if (hk_plan_create(_rt, NULL, &_plan) != HK_STATUS_OK || !_plan) return -1;",
        "    hk_objc_target_t _t = _isMeta ? hk_objc_class_method(_cls, _sel) : hk_objc_instance_method(_cls, _sel);",
        "    // _t.inheritance_policy is HK_OBJC_LOCAL_METHOD_ONLY by default (safe).",
        "    const char *_sid = (_stable_hook_id && _stable_hook_id[0]) ? _stable_hook_id : \"logos.hook\";",
        "    hk_hook_spec_t _spec; hk_objc_spec_init(&_spec, _sid, _t, (void*)_new);",
        "    hk_hook_t *_hook = NULL; if (hk_plan_add_hook(_plan, &_spec, &_hook) != HK_STATUS_OK) { hk_plan_release(_plan); return -1; }",
        "    (void)hk_plan_analyze(_plan, NULL); (void)hk_plan_prepare(_plan, NULL); (void)hk_plan_commit(_plan, NULL);",
        "    int _ok = -1; if (_hook) { hk_hook_result_t _r; memset(&_r,0,sizeof(_r)); _r.struct_size=sizeof(_r); _r.struct_version=HK_ABI_VERSION_3_0;",
        "        if (hk_hook_copy_result(_hook,&_r)==HK_STATUS_OK && (_r.outcome==HK_OUTCOME_ACTIVE||_r.outcome==HK_OUTCOME_ALREADY_ACTIVE)) {",
        "            _ok=0; if (_old) { void *_o=(void*)hk_original_slot_load(hk_hook_original_slot(_hook)); if(_o) *_old=(IMP)_o; }",
        "        }",
        "    }",
        "    hk_plan_release(_plan); return _ok;",
        "}",
        "__attribute__((unused)) static int _hk_hookkit_hook_message_allow_inherited(const char *_stable_hook_id, Class _cls, SEL _sel, IMP _new, IMP *_old, int _isMeta) {",
        "    if (!_cls || !_sel || !_new) return -1;",
        "    hk_runtime_t *_rt = _hk_hookkit_shared_runtime(); if (!_rt) return -1;",
        "    hk_plan_t *_plan = NULL; if (hk_plan_create(_rt, NULL, &_plan) != HK_STATUS_OK || !_plan) return -1;",
        "    hk_objc_target_t _t = _isMeta ? hk_objc_class_method(_cls, _sel) : hk_objc_instance_method(_cls, _sel); _t.inheritance_policy = HK_OBJC_ALLOW_INHERITED_OVERRIDE;",
        "    const char *_sid2 = (_stable_hook_id && _stable_hook_id[0]) ? _stable_hook_id : \"logos.hook\";",
        "    hk_hook_spec_t _spec2; hk_objc_spec_init(&_spec2, _sid2, _t, (void*)_new);",
        "    hk_hook_t *_hook2 = NULL; if (hk_plan_add_hook(_plan, &_spec2, &_hook2) != HK_STATUS_OK) { hk_plan_release(_plan); return -1; }",
        "    (void)hk_plan_analyze(_plan, NULL); (void)hk_plan_prepare(_plan, NULL); (void)hk_plan_commit(_plan, NULL);",
        "    int _ok2 = -1; if (_hook2) { hk_hook_result_t _r2; memset(&_r2,0,sizeof(_r2)); _r2.struct_size=sizeof(_r2); _r2.struct_version=HK_ABI_VERSION_3_0;",
        "        if (hk_hook_copy_result(_hook2,&_r2)==HK_STATUS_OK && (_r2.outcome==HK_OUTCOME_ACTIVE||_r2.outcome==HK_OUTCOME_ALREADY_ACTIVE)) {",
        "            _ok2=0; if (_old) { void *_o2=(void*)hk_original_slot_load(hk_hook_original_slot(_hook2)); if(_o2) *_old=(IMP)_o2; }",
        "        }",
        "    }",
        "    hk_plan_release(_plan); return _ok2;",
        "}",
        "__attribute__((unused)) static int _hk_hookkit_hook_function(const char *_stable_hook_id, void *_sym, void *_rep, void **_out) {",
        "    if (!_sym || !_rep) return -1;",
        "    hk_runtime_t *_rt = _hk_hookkit_shared_runtime(); if (!_rt) return -1;",
        "    hk_plan_t *_plan = NULL; if (hk_plan_create(_rt, NULL, &_plan) != HK_STATUS_OK || !_plan) return -1;",
        "    hk_hook_spec_t _spec; memset(&_spec,0,sizeof(_spec)); _spec.struct_size=sizeof(_spec); _spec.struct_version=HK_ABI_VERSION_3_0;",
        "    _spec.stable_hook_id=(_stable_hook_id && _stable_hook_id[0]) ? _stable_hook_id : \"logos.func\"; _spec.target_kind=HK_TARGET_FUNCTION_ADDRESS;",
        "    _spec.target.address.struct_size=sizeof(_spec.target.address); _spec.target.address.struct_version=HK_ABI_VERSION_3_0;",
        "    _spec.target.address.address=(uintptr_t)_sym; _spec.replacement=_rep;",
        "    _spec.required_reach=HK_REACH_ENTRYPOINT; _spec.original_requirement=HK_ORIGINAL_CALLABLE_CONTINUATION;",
        "    _spec.continuation_policy=HK_CONTINUATION_ANY; _spec.availability=HK_AVAILABILITY_REQUIRED_NOW; _spec.role=HK_OPERATION_MANDATORY;",
        "    hk_hook_t *_hook=NULL; if(hk_plan_add_hook(_plan,&_spec,&_hook)!=HK_STATUS_OK){hk_plan_release(_plan);return -1;}",
        "    (void)hk_plan_analyze(_plan,NULL);(void)hk_plan_prepare(_plan,NULL);(void)hk_plan_commit(_plan,NULL);",
        "    int _ok=-1; if(_hook){hk_hook_result_t _r; memset(&_r,0,sizeof(_r)); _r.struct_size=sizeof(_r); _r.struct_version=HK_ABI_VERSION_3_0;",
        "        if(hk_hook_copy_result(_hook,&_r)==HK_STATUS_OK && (_r.outcome==HK_OUTCOME_ACTIVE||_r.outcome==HK_OUTCOME_ALREADY_ACTIVE)){_ok=0; if(_out){void *_o=(void*)hk_original_slot_load(hk_hook_original_slot(_hook)); if(_o) *_out=_o;}}}",
        "    hk_plan_release(_plan); return _ok;",
        "}",
        "",
    ));
}

1;
