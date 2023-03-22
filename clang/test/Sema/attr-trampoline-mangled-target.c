// RUN: %clang_cc1 %s -verify -fsyntax-only

__attribute__((trampoline_mangled_target("foo")))
void correct(void) {}

__attribute__((trampoline_mangled_target())) // expected-error {{'trampoline_mangled_target' attribute takes one argument}}
void no_arg(void) {}

__attribute__((trampoline_mangled_target(1))) // expected-error {{'trampoline_mangled_target' attribute requires a string}}
void wrong_arg(void) {}

__attribute__((trampoline_mangled_target("foo"))) // expected-note {{conflicting attribute is here}}
void different_targets(void);

__attribute__((trampoline_mangled_target("bar"))) // expected-error {{'trampoline_mangled_target' and 'trampoline_mangled_target' attributes are not compatible}}
void different_targets(void) {}

