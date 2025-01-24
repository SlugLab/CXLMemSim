int main(){
	asm(    "\txor %%ecx,%%ecx\n"
        "\tmov $2000000000,%%ecx\n"
        "test_loop:\n"
        "\tjmp test_jmp\n"
        "\tnop\n"
        "test_jmp:\n"
        "\txor %%eax,%%eax\n"
        "\tjnz test_jmp2\n"
        "\tinc %%eax\n"
        "test_jmp2:\n"
        "\tdec %%ecx\n"
        "\tjnz test_loop\n"
        : /* no output registers */
        : /* no inputs */
        : "cc", "%ecx", "%eax" /* clobbered */
    	);
        return 0;
}
