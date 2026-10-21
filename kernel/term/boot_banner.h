/* SutureOS: post-boot system banner (see boot_banner.c). */
#ifndef SUTUREOS_BOOT_BANNER_H
#define SUTUREOS_BOOT_BANNER_H

/* Clear the scrolling boot log and print the system name banner.
 * Call this once, right before the shell takes over the screen. */
void boot_banner_draw(void);

#endif /* SUTUREOS_BOOT_BANNER_H */
