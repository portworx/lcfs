// +build linux

package dfs

import (
    "fmt"
    "path"
    "syscall"
    "unsafe"

    "github.com/Sirupsen/logrus"
    "github.com/docker/docker/daemon/graphdriver"
    "github.com/docker/docker/pkg/idtools"
)

// Copied from dfs.h
const (
    SnapCreate = 101
    CloneCreate = 102
    SnapRemove = 103
    SnapMount = 104
    SnapUmount = 105
    SnapStat = 106
    UmountAll = 107
)

func init() {
    graphdriver.Register("dfs", Init)
}

// Init returns a new DFS driver.
// An error is returned if DFS is not supported.
func Init(home string, options []string, uidMaps, gidMaps []idtools.IDMap) (graphdriver.Driver, error) {
    rootUID, rootGID, err := idtools.GetRootUIDGID(uidMaps, gidMaps)
    if err != nil {
        return nil, err
    }
    if err := idtools.MkdirAllAs(home, 0700, rootUID, rootGID); err != nil {
        return nil, err
    }

    driver := &Driver{
        home:    home,
        uidMaps: uidMaps,
        gidMaps: gidMaps,
    }

    logrus.Infof("dfs Initialized at %s", home)
    return graphdriver.NewNaiveDiffDriver(driver, uidMaps, gidMaps), nil
}


// Driver contains information about the filesystem mounted.
type Driver struct {
    //root of the file system
    home    string
    uidMaps []idtools.IDMap
    gidMaps []idtools.IDMap
}

// String prints the name of the driver (dfs).
func (d *Driver) String() string {
    return "dfs"
}

// Status returns current driver information in a two dimensional string array.
// Output contains "Build Version" and "Library Version" of the dfs libraries used.
// Version information can be used to check compatibility with your kernel.
func (d *Driver) Status() [][2]string {
    status := [][2]string{}
    if bv := dfsBuildVersion(); bv != "-" {
        status = append(status, [2]string{"Build Version", bv})
    }
    if lv := dfsLibVersion(); lv != -1 {
        status = append(status, [2]string{"Library Version", fmt.Sprintf("%d", lv)})
    }
    return status
}

// GetMetadata returns empty metadata for this driver.
func (d *Driver) GetMetadata(id string) (map[string]string, error) {
    return nil, nil
}

// Issue ioctl for various operations
func (d *Driver) ioctl(cmd int, parent, id string) error {
    var op, arg uintptr
    var name string
    var plen int

    logrus.Debugf("dfs ioctl cmd %d parent %s id %s", cmd, parent, id)
    // Open snapshot root directory
    fd, err := syscall.Open(d.home, syscall.O_DIRECTORY, 0);
    if err != nil {
        return err
    }

    // Create a name string which includes both parent and id 
    if parent != "" {
        name = path.Join(parent, id)
        plen = len(parent)
    } else {
        name = id
    }
    if name == "" {
        op = uintptr(cmd)
    } else {
        op = uintptr((1 << 30) | (len(name) << 16) | (plen << 8) | cmd);
        arg = uintptr(unsafe.Pointer(&[]byte(name)[0]))
    }
    _, _, ep := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), op, arg);
    err = syscall.Close(fd)
    if ep != 0 {
        return syscall.Errno(ep)
    }
    return err
}

// Cleanup unmounts the home directory.
func (d *Driver) Cleanup() error {
    return d.ioctl(UmountAll, "", "")
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (d *Driver) CreateReadWrite(id, parent string, opts *graphdriver.CreateOpts) error {
    return d.ioctl(CloneCreate, parent, id)
}

// Create the filesystem with given id.
func (d *Driver) Create(id, parent string, opts *graphdriver.CreateOpts) error {
    return d.ioctl(SnapCreate, parent, id)
}

// Remove the filesystem with given id.
func (d *Driver) Remove(id string) error {
    return d.ioctl(SnapRemove, "", id)
}

// Get the requested filesystem id.
func (d *Driver) Get(id, mountLabel string) (string, error) {
    dir := path.Join(d.home, id)
    err := d.ioctl(SnapMount, "", id)
    if err != nil {
        return "", err
    }
    return dir, nil
}

// Put is kind of unmounting the file system.
func (d *Driver) Put(id string) error {
    return d.ioctl(SnapUmount, "", id)
}

// Exists checks if the id exists in the filesystem.
func (d *Driver) Exists(id string) bool {
    err := d.ioctl(SnapStat, "", id)
    return err == nil
}
