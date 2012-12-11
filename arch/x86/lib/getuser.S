/*
 * __get_user functions.
 *
 * (C) Copyright 1998 Linus Torvalds
 * (C) Copyright 2005 Andi Kleen
 * (C) Copyright 2008 Glauber Costa
 *
 * These functions have a non-standard call interface
 * to make them more efficient, especially as they
 * return an error value in addition to the "real"
 * return value.
 */

/*
 * __get_user_X
 *
 * Inputs:	%[r|e]cx contains the address.
 *
 * Outputs:	%[r|e]cx is error code (0 or -EFAULT)
 *		%[r|e]ax contains zero-extended value
 *		%edx contains the high bits of the value for __get_user_8 on x86-32
 *
 *
 * These functions should not modify any other registers,
 * as they get called from within inline assembly.
 */

#include <linux/linkage.h>
#include <asm/dwarf2.h>
#include <asm/page_types.h>
#include <asm/errno.h>
#include <asm/asm-offsets.h>
#include <asm/thread_info.h>
#include <asm/asm.h>
#include <asm/smap.h>

	.text
ENTRY(__get_user_1)
	CFI_STARTPROC
	GET_THREAD_INFO(%_ASM_AX)
	cmp TI_addr_limit(%_ASM_AX),%_ASM_CX
	jae bad_get_user
	ASM_STAC
1:	movzb (%_ASM_CX),%eax
	xor %ecx,%ecx
	ASM_CLAC
	ret
	CFI_ENDPROC
ENDPROC(__get_user_1)

ENTRY(__get_user_2)
	CFI_STARTPROC
	add $1,%_ASM_CX
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_AX)
	cmp TI_addr_limit(%_ASM_AX),%_ASM_CX
	jae bad_get_user
	ASM_STAC
2:	movzwl -1(%_ASM_CX),%eax
	xor %ecx,%ecx
	ASM_CLAC
	ret
	CFI_ENDPROC
ENDPROC(__get_user_2)

ENTRY(__get_user_4)
	CFI_STARTPROC
	add $3,%_ASM_CX
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_AX)
	cmp TI_addr_limit(%_ASM_AX),%_ASM_CX
	jae bad_get_user
	ASM_STAC
3:	mov -3(%_ASM_CX),%eax
	xor %ecx,%ecx
	ASM_CLAC
	ret
	CFI_ENDPROC
ENDPROC(__get_user_4)

#ifdef CONFIG_X86_64
ENTRY(__get_user_8)
	CFI_STARTPROC
	add $7,%_ASM_CX
	jc bad_get_user
	GET_THREAD_INFO(%_ASM_AX)
	cmp TI_addr_limit(%_ASM_AX),%_ASM_CX
	jae	bad_get_user
	ASM_STAC
4:	movq -7(%_ASM_CX),%_ASM_AX
	xor %ecx,%ecx
	ASM_CLAC
	ret
	CFI_ENDPROC
ENDPROC(__get_user_8)
#else
ENTRY(__get_user_8)
	CFI_STARTPROC
	add $7,%_ASM_CX
	jc bad_get_user_8
	GET_THREAD_INFO(%_ASM_AX)
	cmp TI_addr_limit(%_ASM_AX),%_ASM_CX
	jae	bad_get_user_8
	ASM_STAC
4:	mov -7(%_ASM_CX),%eax
5:	mov -3(%_ASM_CX),%edx
	xor %ecx,%ecx
	ASM_CLAC
	ret
	CFI_ENDPROC
ENDPROC(__get_user_8)
#endif

bad_get_user:
	CFI_STARTPROC
	xor %eax,%eax
	mov $(-EFAULT),%_ASM_CX
	ASM_CLAC
	ret
	CFI_ENDPROC
END(bad_get_user)

#ifdef CONFIG_X86_32
bad_get_user_8:
	CFI_STARTPROC
	xor %eax,%eax
	xor %edx,%edx
	mov $(-EFAULT),%_ASM_CX
	ASM_CLAC
	ret
	CFI_ENDPROC
END(bad_get_user_8)
#endif

	_ASM_EXTABLE(1b,bad_get_user)
	_ASM_EXTABLE(2b,bad_get_user)
	_ASM_EXTABLE(3b,bad_get_user)
#ifdef CONFIG_X86_64
	_ASM_EXTABLE(4b,bad_get_user)
#else
	_ASM_EXTABLE(4b,bad_get_user_8)
	_ASM_EXTABLE(5b,bad_get_user_8)
#endif
