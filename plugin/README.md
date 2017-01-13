# Setting up the Px-Graph graphdriver plugin

### Step 1: Install needed packages
Set up GOPATH and checkout this directory under GOPATH.
```
# go get -d github.com:/portworx/px-graph/...
```

### Step 2: Create a v2 portworx/px-graph graphdriver plugin
> Note: Make sure lcfs installed and configured as per [these instructions](https://github.com/portworx/px-graph/blob/master/lcfs/README.md) before installing Px-Graph.

Now, you can run the setup script from this directory
```
# cd $GOPATH/src/github.com/portworx/plugin
# ./setup.sh
```

At this point, Docker will be available to use `portworx/px-graph` as a graph driver option.
