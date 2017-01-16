# Building the LCFS graphdriver plugin
These instructions describe how to build the LCFS graphdriver plugin.  The graphdriver plugin implements the Docker V2 plugin interface so that Docker can use the LCFS filesystem for storing container layers.

### Step 1: Install needed packages
Set up GOPATH and checkout this directory under GOPATH.
```
# go get -d github.com:/portworx/lcfs/...
```

### Step 2: Create a v2 portworx/lcfs graphdriver plugin
> Note: Make sure lcfs installed and configured as per [these instructions](https://github.com/portworx/lcfs/blob/master/lcfs/README.md) before installing the LCFS plugin.

Now, you can run the setup script from this directory
```
# cd $GOPATH/src/github.com/portworx/lcfs/plugin
# make
```

At this point, Docker will be available to use LCFS as a graph driver.
