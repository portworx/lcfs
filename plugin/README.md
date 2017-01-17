# Building the LCFS graphdriver plugin
The graphdriver plugin implements the Docker V2 plugin interface so that Docker can use the LCFS file system for storing container layers.

Just run [plugin/setup.sh](https://github.com/portworx/lcfs/blob/master/plugin/setup.sh) to build and install the LCFS plugin.

**Note that you must have followed the steps to install the LCFS file system at /lcfs prior to running this script.**
