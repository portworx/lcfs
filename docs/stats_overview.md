# Stats

When enabled at build time, all file operations and ioctl requests are counted and times taken for each of them are tracked for each layer separately. These stats can be queried using a command. Currently, they are also displayed at the time that a layer is unmounted. Stats for a layer can be cleared before running applications to trace actual operations during any time period.

Memory usage on a per-layer basis is tracked and reported as well. Similarly, a count of files of different types in every layer is maintained. The count of I/Os issued by each layer is also tracked.
