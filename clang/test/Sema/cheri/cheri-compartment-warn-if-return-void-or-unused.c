// RUN: %clang_cc1 %s -o - -triple riscv32-unknown-unknown -emit-llvm -mframe-pointer=none -mcmodel=small -target-cpu cheriot -target-feature +xcheri -target-feature -64bit -target-feature -relax -target-feature -xcheri-rvc -target-feature -save-restore -target-abi cheriot -Oz -cheri-compartment=example -fsyntax-only -verify

__attribute__((cheri_compartment("example"))) void void_return_type_f() // expected-warning{{attribute 'cheri_compartment' cannot be applied to functions without return value}} expected-warning{{attribute 'cheri_compartment' cannot be applied to functions without return value}}
{
}

__attribute__((cheri_compartment("example"))) int int_return_type_f() {
  return 0;
}

void unused_int_return_type_f() {
  int_return_type_f(); // expected-warning{{ignoring return value of function declared with 'nodiscard' attribute: CHERI compartment call}}
}
