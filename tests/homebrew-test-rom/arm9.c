/* A minimal, fully original *interactive* homebrew program: continuously
 * reads the real NDS KEYINPUT register and reflects currently-held
 * buttons as a solid backdrop color on ENGINE B (confirmed to be what
 * melonDS's GPU::GetFramebuffers() "bottom" pointer delivers by default),
 * so a remote client's DS button presses -- routed through melonDS's
 * SetKeyMask() -- become immediately, visibly observable in the delivered
 * bottom-screen frame. Used to verify SPEC.md section 20 acceptance
 * criteria (4)-(8): that DS controls sent over the remote protocol
 * actually affect a running program, not just that a static frame gets
 * delivered. See tests/homebrew-test-rom/README.md for the full
 * end-to-end test results (byte order, engine mapping, per-button
 * response) obtained by driving this ROM through the real network
 * pipeline.
 *
 * KEYINPUT (0x04000130) is a public, documented NDS hardware register
 * (see GBATEK); bits are active-low (0 = pressed):
 *   bit0=A bit1=B bit2=Select bit3=Start bit4=Right bit5=Left
 *   bit6=Up bit7=Down bit8=R bit9=L
 *
 * NOTE: both 2D engines' DISPCNT are set to display-mode 1, even though
 * only engine B's backdrop is driven dynamically -- empirically, setting
 * only one engine's DISPCNT was unreliable (captured frames stayed
 * white), while setting both consistently worked. Not fully root-caused
 * (possibly related to GPU2D's DispCntLatch being a few-frame-deep latch
 * chain); see docs/known-limitations.md.
 */

void _start(void)
{
    volatile unsigned int *dispcntA = (volatile unsigned int *)0x04000000;
    volatile unsigned int *dispcntB = (volatile unsigned int *)0x04001000;
    volatile unsigned short *engineA_backdrop = (volatile unsigned short *)0x05000000;
    volatile unsigned short *engineB_backdrop = (volatile unsigned short *)0x05000400;
    volatile unsigned short *keyinput = (volatile unsigned short *)0x04000130;

    *dispcntA = 0x00010000; /* display mode 1 = regular 2D graphics */
    *dispcntB = 0x00010000;
    *engineA_backdrop = 0x39CE; /* fixed neutral gray, engine A is unused for input feedback */

    for (;;)
    {
        unsigned short keys = *keyinput;

        /* active-low: bit clear (0) means pressed */
        unsigned short r = (keys & (1 << 0)) ? 0 : 31; /* A     -> red   */
        unsigned short g = (keys & (1 << 1)) ? 0 : 31; /* B     -> green */
        unsigned short b = (keys & (1 << 6)) ? 0 : 31; /* Up    -> blue  */

        *engineB_backdrop = r | (g << 5) | (b << 10);
    }
}
