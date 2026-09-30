/* NexIOS demonstration program: clean user-task execution.
 *
 * Loaded via `load /hey.c.elf` + bare `runelf`: the background ElfLoader
 * maps it as an ET_EXEC user image, runelf admits + dispatches it, and it
 * prints through the WRITE path and exits 0.
 */
#include <stdio.h>

int main(void) {
    printf("Hey! This is NexIOS, running your application!\n");
    return 0;
}
