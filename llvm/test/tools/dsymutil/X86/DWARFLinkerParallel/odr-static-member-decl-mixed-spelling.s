# Two compile units describe the same struct S, but they disagree on how the
# in-class static data member is spelled: a.cpp uses the DWARF 4 / GCC form
# (DW_TAG_member with DW_AT_declaration) and b.cpp uses the DWARF 5 form
# (DW_TAG_variable with DW_AT_declaration). Such a mix shows up whenever
# objects built with different -gdwarf-version values, or by different
# producers, are linked together.
#
# The static declaration is not a data member, so it must not shift the
# positions the parallel linker derives the names of the real data members
# from. When two units disagree on those positions they derive different names
# for the same field, so the deduplicated struct gains a DW_TAG_member per
# name; and a position claimed by a static declaration in one unit and by a
# data member in another merges the two into a single DIE.
#
# The hand-written DWARF below describes:
#   struct S {
#     static const int kMask = 1;
#     int field;
#   };
# referenced from useA() in a.cpp and useB() in b.cpp.

# RUN: llvm-mc -triple x86_64-apple-darwin -filetype=obj %s -o %t.o
# RUN: llvm-dwarfdump --verify %t.o

# RUN: echo '---' > %t.map
# RUN: echo "triple:          'x86_64-apple-darwin'" >> %t.map
# RUN: echo 'objects:'  >> %t.map
# RUN: echo " -  filename: '%t.o'" >> %t.map
# RUN: echo '    symbols:' >> %t.map
# RUN: echo '      - { sym: __Z4useAP1S, objAddr: 0x0, binAddr: 0x10000, size: 0x4 }' >> %t.map
# RUN: echo '      - { sym: __Z4useBP1S, objAddr: 0x4, binAddr: 0x10010, size: 0x4 }' >> %t.map
# RUN: echo '...' >> %t.map

# RUN: dsymutil --linker=parallel -y %t.map -f -o %t.parallel.dSYM
# RUN: llvm-dwarfdump --verify %t.parallel.dSYM
# RUN: llvm-dwarfdump --debug-info %t.parallel.dSYM \
# RUN:   | FileCheck %s --check-prefix=PARALLEL \
# RUN:                  --implicit-check-not='DW_AT_name{{.*}}"field"'

# RUN: dsymutil --linker=classic -y %t.map -f -o %t.classic.dSYM
# RUN: llvm-dwarfdump --verify %t.classic.dSYM
# RUN: llvm-dwarfdump --debug-info %t.classic.dSYM \
# RUN:   | FileCheck %s --check-prefix=CLASSIC

# The parallel linker moves S into the artificial type unit. The
# --implicit-check-not above enforces that the whole output names "field"
# exactly once, i.e. that S has a single DW_TAG_member for it. The kMask checks
# enforce that the static declaration is still there rather than displaced by
# the field.
#
# Both spellings of kMask survive, since they are distinct DIEs as far as type
# deduplication is concerned. That is harmless: a static data member has no
# storage inside the record.

# PARALLEL: DW_TAG_compile_unit
# PARALLEL: DW_AT_name{{.*}}"__artificial_type_unit"
# PARALLEL: DW_TAG_structure_type
# PARALLEL: DW_AT_name{{.*}}"S"
# PARALLEL: DW_AT_name{{.*}}"kMask"
# PARALLEL: DW_AT_declaration
# PARALLEL: DW_TAG_member
# PARALLEL: DW_AT_name{{.*}}"field"
# PARALLEL: DW_AT_data_member_location{{.*}}(0x00)

# PARALLEL: DW_TAG_compile_unit
# PARALLEL: DW_AT_name{{.*}}"a.cpp"
# PARALLEL: DW_AT_name{{.*}}"useA"

# PARALLEL: DW_TAG_compile_unit
# PARALLEL: DW_AT_name{{.*}}"b.cpp"
# PARALLEL: DW_AT_name{{.*}}"useB"

# The classic linker does not derive type names from child positions, so it is
# unaffected. Running it here guards against regressions and confirms the
# inputs aren't pathological.

# CLASSIC: DW_TAG_structure_type
# CLASSIC: DW_AT_name{{.*}}"S"
# CLASSIC: DW_TAG_member
# CLASSIC: DW_AT_name{{.*}}"field"

	.section	__TEXT,__text,regular,pure_instructions
	.globl	__Z4useAP1S
__Z4useAP1S:
LfuncA_begin:
	retq
	nop
	nop
	nop
LfuncA_end:

	.globl	__Z4useBP1S
__Z4useBP1S:
LfuncB_begin:
	retq
	nop
	nop
	nop
LfuncB_end:

	.section	__DWARF,__debug_abbrev,regular,debug
Lsection_abbrev:
	.byte	1                       ## Abbreviation Code
	.byte	17                      ## DW_TAG_compile_unit
	.byte	1                       ## DW_CHILDREN_yes
	.byte	37                      ## DW_AT_producer
	.byte	8                       ## DW_FORM_string
	.byte	19                      ## DW_AT_language
	.byte	5                       ## DW_FORM_data2
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	0, 0

	.byte	2                       ## Abbreviation Code
	.byte	46                      ## DW_TAG_subprogram
	.byte	1                       ## DW_CHILDREN_yes
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	110                     ## DW_AT_linkage_name
	.byte	8                       ## DW_FORM_string
	.byte	17                      ## DW_AT_low_pc
	.byte	1                       ## DW_FORM_addr
	.byte	18                      ## DW_AT_high_pc
	.byte	1                       ## DW_FORM_addr
	.byte	63                      ## DW_AT_external
	.byte	12                      ## DW_FORM_flag
	.byte	0, 0

	.byte	3                       ## Abbreviation Code
	.byte	5                       ## DW_TAG_formal_parameter
	.byte	0                       ## DW_CHILDREN_no
	.byte	73                      ## DW_AT_type
	.byte	19                      ## DW_FORM_ref4
	.byte	0, 0

	.byte	4                       ## Abbreviation Code
	.byte	19                      ## DW_TAG_structure_type
	.byte	1                       ## DW_CHILDREN_yes
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	11                      ## DW_AT_byte_size
	.byte	11                      ## DW_FORM_data1
	.byte	0, 0

	.byte	5                       ## Abbreviation Code
	.byte	13                      ## DW_TAG_member
	.byte	0                       ## DW_CHILDREN_no
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	73                      ## DW_AT_type
	.byte	19                      ## DW_FORM_ref4
	.byte	56                      ## DW_AT_data_member_location
	.byte	11                      ## DW_FORM_data1
	.byte	0, 0

	.byte	6                       ## Abbreviation Code
	.byte	13                      ## DW_TAG_member (static data member, DWARF 4)
	.byte	0                       ## DW_CHILDREN_no
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	73                      ## DW_AT_type
	.byte	19                      ## DW_FORM_ref4
	.byte	63                      ## DW_AT_external
	.byte	12                      ## DW_FORM_flag
	.byte	60                      ## DW_AT_declaration
	.byte	12                      ## DW_FORM_flag
	.byte	28                      ## DW_AT_const_value
	.byte	11                      ## DW_FORM_data1
	.byte	0, 0

	.byte	7                       ## Abbreviation Code
	.byte	52                      ## DW_TAG_variable (static data member, DWARF 5)
	.byte	0                       ## DW_CHILDREN_no
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	73                      ## DW_AT_type
	.byte	19                      ## DW_FORM_ref4
	.byte	63                      ## DW_AT_external
	.byte	12                      ## DW_FORM_flag
	.byte	60                      ## DW_AT_declaration
	.byte	12                      ## DW_FORM_flag
	.byte	28                      ## DW_AT_const_value
	.byte	11                      ## DW_FORM_data1
	.byte	0, 0

	.byte	8                       ## Abbreviation Code
	.byte	36                      ## DW_TAG_base_type
	.byte	0                       ## DW_CHILDREN_no
	.byte	3                       ## DW_AT_name
	.byte	8                       ## DW_FORM_string
	.byte	11                      ## DW_AT_byte_size
	.byte	11                      ## DW_FORM_data1
	.byte	62                      ## DW_AT_encoding
	.byte	11                      ## DW_FORM_data1
	.byte	0, 0

	.byte	9                       ## Abbreviation Code
	.byte	15                      ## DW_TAG_pointer_type
	.byte	0                       ## DW_CHILDREN_no
	.byte	73                      ## DW_AT_type
	.byte	19                      ## DW_FORM_ref4
	.byte	0, 0

	.byte	0                       ## EOM(3)

	.section	__DWARF,__debug_info,regular,debug
Lsection_info:
## DWARF 4 compile unit: the static data member is a DW_TAG_member.
Lcu1_begin:
	.long	Lcu1_end - Lcu1_start   ## Length of Unit
Lcu1_start:
	.short	4                       ## DWARF version number
	.long	0                       ## Offset Into Abbrev. Section
	.byte	8                       ## Address Size (in bytes)

	.byte	1                       ## Abbrev [1] DW_TAG_compile_unit
	.asciz	"hand-written"          ## DW_AT_producer
	.short	0x0004                  ## DW_AT_language (DW_LANG_C_plus_plus)
	.asciz	"a.cpp"                 ## DW_AT_name

	.byte	2                       ## Abbrev [2] DW_TAG_subprogram
	.asciz	"useA"                  ## DW_AT_name
	.asciz	"__Z4useAP1S"           ## DW_AT_linkage_name
	.quad	LfuncA_begin            ## DW_AT_low_pc
	.quad	LfuncA_end              ## DW_AT_high_pc
	.byte	1                       ## DW_AT_external

	.byte	3                       ## Abbrev [3] DW_TAG_formal_parameter
	.long	Lcu1_s_ptr - Lcu1_begin ## DW_AT_type

	.byte	0                       ## End Of Children Mark (useA)

Lcu1_s:
	.byte	4                       ## Abbrev [4] DW_TAG_structure_type
	.asciz	"S"                     ## DW_AT_name
	.byte	4                       ## DW_AT_byte_size

	.byte	6                       ## Abbrev [6] DW_TAG_member
	.asciz	"kMask"                 ## DW_AT_name
	.long	Lcu1_int - Lcu1_begin   ## DW_AT_type
	.byte	1                       ## DW_AT_external
	.byte	1                       ## DW_AT_declaration
	.byte	1                       ## DW_AT_const_value

	.byte	5                       ## Abbrev [5] DW_TAG_member
	.asciz	"field"                 ## DW_AT_name
	.long	Lcu1_int - Lcu1_begin   ## DW_AT_type
	.byte	0                       ## DW_AT_data_member_location

	.byte	0                       ## End Of Children Mark (S)

Lcu1_int:
	.byte	8                       ## Abbrev [8] DW_TAG_base_type
	.asciz	"int"                   ## DW_AT_name
	.byte	4                       ## DW_AT_byte_size
	.byte	5                       ## DW_AT_encoding (DW_ATE_signed)

Lcu1_s_ptr:
	.byte	9                       ## Abbrev [9] DW_TAG_pointer_type
	.long	Lcu1_s - Lcu1_begin     ## DW_AT_type

	.byte	0                       ## End Of Children Mark (CU)
Lcu1_end:

## DWARF 5 compile unit: the static data member is a DW_TAG_variable.
Lcu2_begin:
	.long	Lcu2_end - Lcu2_start   ## Length of Unit
Lcu2_start:
	.short	5                       ## DWARF version number
	.byte	1                       ## DW_UT_compile
	.byte	8                       ## Address Size (in bytes)
	.long	0                       ## Offset Into Abbrev. Section

	.byte	1                       ## Abbrev [1] DW_TAG_compile_unit
	.asciz	"hand-written"          ## DW_AT_producer
	.short	0x0004                  ## DW_AT_language (DW_LANG_C_plus_plus)
	.asciz	"b.cpp"                 ## DW_AT_name

	.byte	2                       ## Abbrev [2] DW_TAG_subprogram
	.asciz	"useB"                  ## DW_AT_name
	.asciz	"__Z4useBP1S"           ## DW_AT_linkage_name
	.quad	LfuncB_begin            ## DW_AT_low_pc
	.quad	LfuncB_end              ## DW_AT_high_pc
	.byte	1                       ## DW_AT_external

	.byte	3                       ## Abbrev [3] DW_TAG_formal_parameter
	.long	Lcu2_s_ptr - Lcu2_begin ## DW_AT_type

	.byte	0                       ## End Of Children Mark (useB)

Lcu2_s:
	.byte	4                       ## Abbrev [4] DW_TAG_structure_type
	.asciz	"S"                     ## DW_AT_name
	.byte	4                       ## DW_AT_byte_size

	.byte	7                       ## Abbrev [7] DW_TAG_variable
	.asciz	"kMask"                 ## DW_AT_name
	.long	Lcu2_int - Lcu2_begin   ## DW_AT_type
	.byte	1                       ## DW_AT_external
	.byte	1                       ## DW_AT_declaration
	.byte	1                       ## DW_AT_const_value

	.byte	5                       ## Abbrev [5] DW_TAG_member
	.asciz	"field"                 ## DW_AT_name
	.long	Lcu2_int - Lcu2_begin   ## DW_AT_type
	.byte	0                       ## DW_AT_data_member_location

	.byte	0                       ## End Of Children Mark (S)

Lcu2_int:
	.byte	8                       ## Abbrev [8] DW_TAG_base_type
	.asciz	"int"                   ## DW_AT_name
	.byte	4                       ## DW_AT_byte_size
	.byte	5                       ## DW_AT_encoding (DW_ATE_signed)

Lcu2_s_ptr:
	.byte	9                       ## Abbrev [9] DW_TAG_pointer_type
	.long	Lcu2_s - Lcu2_begin     ## DW_AT_type

	.byte	0                       ## End Of Children Mark (CU)
Lcu2_end:
