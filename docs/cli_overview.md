# Growing LCFS file system

When the underlying device/file is extended in size, the LCFS can make use of
the additional space after running the following command.

```
# sudo lcfs grow /lcfs
```

# Adjusting the frequency of LCFS commit (sync) operations

Periodically, LCFS commits its state in memory to disk to make those
persistent.  All operations happened until that point will be saved on disk and
will not be lost even if an abnormal shutdown happens (system crashes and such).
This background activity is triggered every minute by default.  This time
interval could be changed by running the following command.

```
# sudo lcfs syncer /lcfs <time in seconds>
```

This background activity can be disabling by specifying 0 seconds.

# Adjusting the amount of memory for caching images in memory

LCFS caches images in memory in a private page cache.  It is configured to use
512MB or 5% of the system memory (whichever is higher) by default.  This limit
could be changed by running the following command.

```
# sudo lcfs pcache /lcfs <memory limit in MB>
```

# Releasing memory used to cache images

The memory used for caching images (private page cache) could be freed by
running the following command.

```
# sudo lcfs flush /lcfs
```

# Trigger a commit (sync) operation

If needed, all dirty data in memory could be committed to disk by running the
following command.


```
# sudo lcfs commit /lcfs
```

# Options which can be enabled at mount time

A few capabilities of LCFS are not turned on by default for performance
reasons.  Those could be enabled by specifying appropriate options while
mounting the LCFS.  Here is a complete list of options.


```
usage: lcfs daemon <device/file> <host-mountpath> <plugin-mountpath> [-f] [-c] [-d] [-m] [-r] [-t] [-p] [-v]
	device     - device or file - image layers will be saved here
	host-mount - mount point on host
	host-mount - mount point propogated to the plugin
	-f         - run foreground (optional)
	-c         - format file system (optional)
	-d         - display fuse debugging info (optional)
	-m         - enable memory stats (optional)
	-r         - enable request stats (optional)
	-t         - enable tracking count of file types (optional)
	-p         - enable profiling (optional)
	-v         - enable verbose mode (optional)
```

# Stats

Various stats could be displayed by running the following command.

```
# sudo lcfs stats /lcfs <layer id or .> [-c]
```

The stats will be logged into syslog. Stats will be displayed for the layer
specified (layer id), or for all the layers if . is specified as the layer id.

Stats could be cleared before running some experiments by specifying -c option
with the above command.

Stats are not collected by default for performance reasons.  Different types of
stats need to be enabled while mounting the LCFS by specifying the appropriate
options.  Here is a list of stats supported as of now.

## Memory stats

Each malloc() and free() calls made by LCFS is tracked when memory stats are
enabled.  This is enabled by specifying -m option.

## Request stats

All file operations and ioctl requests are counted and times taken for each of
them are tracked for each layer separately when request stats are enabled by
specifying -r option.

## File types

Type of files (regular, directories, symbolic links, other) created in every

# Profiling

If profiling is enabled at mount time, it will be saved under /tmp/lcfs when
LCFS is unmounted.  The saved profiles could be examined using gperftools
commands like pprof.
