/* Minimal ARM7 program: the NDS direct-boot path jumps both CPUs, so this
 * just needs to exist and not crash. No copyrighted content. */

void _start(void)
{
    for (;;) { }
}
