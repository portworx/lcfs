package main

import (
    "log"
    "io"
    "path"
    "syscall"
    "unsafe"

    "github.com/Sirupsen/logrus"
    "github.com/docker/docker/pkg/reexec"
    "github.com/docker/docker/pkg/archive"
    "github.com/docker/docker/pkg/idtools"
    "github.com/docker/docker/daemon/graphdriver"
    graphPlugin "github.com/docker/go-plugins-helpers/graphdriver"
)

const (
	socketAddress = "/run/docker/plugins/lcfs.sock"
)

type pDriver struct {
    home string
}

var _ graphdriver.ProtoDriver = &pDriver{}

// String prints the name of the driver (lcfs).
func (p *pDriver) String() string {
    logrus.Debugf("Graphdriver String")
    return "lcfs"
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (p *pDriver) CreateReadWrite(id, parent string, storageOpt *graphdriver.CreateOpts) error {
    logrus.Debugf("Graphdriver CreateReadWrite - id %s parent %s", id, parent)
    return nil
}

// Create a read-only layer given id.
func (p *pDriver) Create(id, parent string, storageOpt *graphdriver.CreateOpts) error {
    logrus.Debugf("Graphdriver Create - id %s parent %s", id, parent)
    return nil
}

// Remove the layer with given id.
func (p *pDriver) Remove(id string) error {
    logrus.Debugf("Graphdriver Remove id %s", id)
    return nil
}

// Get the requested filesystem id.
func (p *pDriver) Get(id, mountLabel string) (dir string, err error) {
    logrus.Debugf("Graphdriver Get - id %s mountLabel %s", id, mountLabel)
    dir = path.Join(p.home, id)
    return dir, nil
}

// Put is kind of unmounting the file system.
func (p *pDriver) Put(id string) error {
    logrus.Debugf("Graphdriver Put id %s", id)
    return nil
}

// Exists checks if the id exists in the filesystem.
func (p *pDriver) Exists(id string) bool {
    logrus.Debugf("Graphdriver Exists id %s", id)
    return false
}

func (p *pDriver) Status() [][2]string {
    logrus.Debugf("Graphdriver Status")
    return nil
}
func (p *pDriver) GetMetadata(id string) (map[string]string, error) {
    logrus.Debugf("Graphdriver GetMetadata id %s", id)
    return nil, nil
}
func (p *pDriver) Cleanup() error {
    logrus.Debugf("Graphdriver Cleanup")
    return nil
}

// Init initializes the Naive Diff driver
func Init(root string, options []string, uidMaps, gidMaps []idtools.IDMap) (graphdriver.Driver, error) {
    logrus.Debugf("Graphdriver Init - root %s options %+v", root, options)
    d := graphdriver.NewNaiveDiffDriver(&pDriver{home: root}, uidMaps, gidMaps)
    return d, nil
}

// Driver contains information about the filesystem mounted.
type Driver struct {
    driver graphdriver.Driver
    init graphdriver.InitFunc
    home string
    options []string
}

// Copied from lcfs.h
const (
    SnapCreate = 101
    CloneCreate = 102
    SnapRemove = 103
    SnapMount = 104
    SnapUmount = 105
    SnapStat = 106
    UmountAll = 107
)

// Init initializes the storage driver.
func (d *Driver) Init(home string, options []string, uidMaps, gidMaps []idtools.IDMap) error {
    logrus.Infof("Init - home %s options %+v", home, options)
    driver, err := d.init(home, options, nil, nil)
    if err != nil {
        logrus.Errorf("err %v\n", err)
        return err
    }
    d.driver = driver
    d.home = home
    d.options = options
    logrus.Infof("Init - basedir %s", d.home)
    if err := idtools.MkdirAllAs(d.home, 0700, 0, 0); err != nil {
        logrus.Errorf("err %v\n", err)
        return err
    }
    return nil
}

// Issue ioctl for various operations
func (d *Driver) ioctl(cmd int, parent, id string) error {
    var op, arg uintptr
    var name string
    var plen int

    logrus.Debugf("lcfs ioctl cmd %d parent %s id %s", cmd, parent, id)
    // Open snapshot root directory
    fd, err := syscall.Open(d.home, syscall.O_DIRECTORY, 0)
    if err != nil {
        logrus.Errorf("err %v\n", err)
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
        op = uintptr((1 << 30) | (len(name) << 16) | (plen << 8) | cmd)
        arg = uintptr(unsafe.Pointer(&[]byte(name)[0]))
    }
    _, _, ep := syscall.Syscall(syscall.SYS_IOCTL, uintptr(fd), op, arg)
    err = syscall.Close(fd)
    if ep != 0 {
        logrus.Errorf("err %v\n", err)
        return syscall.Errno(ep)
    }
    return err
}

// Create the filesystem with given id.
func (d *Driver) Create(id string, parent string, mountLabel string, storageOpt map[string]string) error {
    logrus.Debugf("Create - id %s parent %s", id, parent)
    return d.ioctl(SnapCreate, parent, id)
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (d *Driver) CreateReadWrite(id string, parent string, mountLabel string, storageOpt map[string]string) error {
    logrus.Debugf("CreateReadWrite - id %s parent %s", id, parent)
    return d.ioctl(CloneCreate, parent, id)
}

// Remove the layer with given id.
func (d *Driver) Remove(id string) error {
    logrus.Debugf("Remove - id %s", id)
    return d.ioctl(SnapRemove, "", id)
}

// Get the requested layer id.
func (d *Driver) Get(id, mountLabel string) (string, error) {
    logrus.Debugf("Get - id %s mountLabel %s", id, mountLabel)
    dir := path.Join(d.home, id)
    err := d.ioctl(SnapMount, "", id)
    if err != nil {
        logrus.Errorf("err %v\n", err)
        return "", err
    }
    return dir, nil
}

// Put is kind of unmounting the layer
func (d *Driver) Put(id string) error {
    logrus.Debugf("Put - id %s ", id)
    return d.ioctl(SnapUmount, "", id)
}

// Exists returns whether a filesystem layer with the specifie
// ID exists on this driver.
func (d *Driver) Exists(id string) bool {
    logrus.Debugf("Exists - id %s", id)
    err := d.ioctl(SnapStat, "", id)
    return err == nil
}

// Status returns current driver information in a two dimensional string array.
// Output contains "Build Version" and "Library Version" of the lcfs libraries used.
// Version information can be used to check compatibility with your kernel.
func (d *Driver) Status() [][2]string {
    logrus.Debugf("Status")
    return [][2]string{[2]string{"Build Version", "1.0"},
                       [2]string{"Library Version", "1.0"}}
}

// GetMetadata returns empty metadata for this driver.
func (d *Driver) GetMetadata(id string) (map[string]string, error) {
    logrus.Debugf("GetMetadata - id %s", id)
    return nil, nil
}

// Cleanup unmounts the home directory.
func (d *Driver) Cleanup() error {
    logrus.Debugf("Cleanup")
    return d.ioctl(UmountAll, "", "")
}

// Diff produces an archive of the changes between the specified
func (d *Driver) Diff(id, parent string) io.ReadCloser {
    logrus.Debugf("Diff - id %s parent %s", id, parent)
    archive, err := d.driver.Diff(id, parent)
    if err != nil {
        logrus.Errorf("Diff: error in stream %v\n", err)
    }
    return archive
}

func changeKind(c archive.ChangeType) graphPlugin.ChangeKind {
    switch c {
    case archive.ChangeModify:
        return graphPlugin.Modified
    case archive.ChangeAdd:
        return graphPlugin.Added
    case archive.ChangeDelete:
        return graphPlugin.Deleted
    }
    return 0
}

// Changes produces a list of changes between the specified layer
// and its parent layer. If parent is "", then all changes will be ADD changes.
func (d *Driver) Changes(id, parent string) ([]graphPlugin.Change, error) {
    logrus.Debugf("Changes - id %s parent %s", id, parent)
    cs, err := d.driver.Changes(id, parent)
    if err != nil {
        logrus.Errorf("Changes: err %v\n", err)
        return nil, err
    }
    changes := make([]graphPlugin.Change, len(cs))
    for _, c := range cs {
        change := graphPlugin.Change{
            Path: c.Path,
            Kind: changeKind(c.Kind),
        }
        changes = append(changes, change)
    }
    return changes, nil
}

// ApplyDiff extracts the changeset from the given diff into the
// layer with the specified id and parent, returning the size of the
// new layer in bytes.
func (d *Driver) ApplyDiff(id, parent string, archive io.Reader) (int64, error) {
    logrus.Debugf("ApplyDiff - id %s parent %s", id, parent)
    return d.driver.ApplyDiff(id, parent, archive)
}

// DiffSize calculates the changes between the specified layer
// and its parent and returns the size in bytes of the changes
// relative to its base filesystem directory.
func (d *Driver) DiffSize(id, parent string) (int64, error) {
    logrus.Debugf("DiffSize - id %s parent %s", id, parent)
    return d.driver.DiffSize(id, parent)
}

func main() {
    if reexec.Init() {
        logrus.Errorf("reexec.Init failed")
        return;
    }
    logrus.SetLevel(logrus.DebugLevel)
    handler := graphPlugin.NewHandler(&Driver{driver: nil, init: Init,
                                      home: "", options: nil})
    logrus.Infof("Starting to serve requests on %s", socketAddress)
    if err := handler.ServeUnix("", socketAddress); err != nil {
        log.Fatalf("Error %v", err)
    }
    for {
    }
}
