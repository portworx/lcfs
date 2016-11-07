package main

import (
    "log"
    "io"
    "path"
    "syscall"
    "unsafe"

    "github.com/Sirupsen/logrus"
    "github.com/docker/docker/pkg/archive"
    "github.com/docker/docker/pkg/idtools"
    "github.com/docker/docker/daemon/graphdriver"
    graphPlugin "github.com/docker/go-plugins-helpers/graphdriver"
)

type pDriver struct {
    home string
}

var _ graphdriver.ProtoDriver = &pDriver{}

func (p *pDriver) String() string {
    logrus.Infof("Graphdriver String\n")
    return "pxg"
}
func (p *pDriver) CreateReadWrite(id, parent, mountLabel string, storageOpt map[string]string) error {
    logrus.Infof("Graphdriver CreateReadWrite - id %s parent %s mountLabel %s\n", id, parent, mountLabel)
    return nil
}
func (p *pDriver) Create(id, parent, mountLabel string, storageOpt map[string]string) error {
    logrus.Infof("Graphdriver Create - id %s parent %s mountLabel %s\n", id, parent, mountLabel)
    return nil
}
func (p *pDriver) Remove(id string) error {
    logrus.Infof("Graphdriver Remove id %s\n", id)
    return nil
}
func (p *pDriver) Get(id, mountLabel string) (dir string, err error) {
    logrus.Infof("Graphdriver Get - id %s mountLabel %s\n", id, mountLabel)
    dir = path.Join(p.home, id)
    logrus.Infof("Graphdriver Get returning dir %s\n", dir)
    return dir, nil
}
func (p *pDriver) Put(id string) error {
    logrus.Infof("Graphdriver Put id %s\n", id)
    return nil
}
func (p *pDriver) Exists(id string) bool {
    logrus.Infof("Graphdriver Exists id %s\n", id)
    return false
}
func (p *pDriver) Status() [][2]string {
    logrus.Infof("Graphdriver Status\n")
    return nil
}
func (p *pDriver) GetMetadata(id string) (map[string]string, error) {
    logrus.Infof("Graphdriver GetMetadata id %s\n", id)
    return nil, nil
}
func (p *pDriver) Cleanup() error {
    logrus.Infof("Graphdriver Cleanup\n")
    return nil
}

func Init(root string, options []string, uidMaps, gidMaps []idtools.IDMap) (graphdriver.Driver, error) {
    logrus.Infof("Graphdriver Init - root %s options %+v\n", root, options)
    d := graphdriver.NewNaiveDiffDriver(&pDriver{home: path.Join(root, "dfs")}, uidMaps, gidMaps)
    return d, nil
}

type Driver struct {
    driver graphdriver.Driver
    init graphdriver.InitFunc
    home string
    options []string
}

// Copied from dfs.h
const (
    SNAP_CREATE = 1
    CLONE_CREATE = 2
    SNAP_REMOVE = 3
    SNAP_MOUNT = 4
    SNAP_UMOUNT = 5
    SNAP_STAT = 6
    UMOUNT_ALL = 7
)

func (d *Driver) Init(home string, options []string) error {
    logrus.Infof("Init - home %s options %+v\n", home, options)
    driver, err := d.init(home, options, nil, nil)
    if err != nil {
        logrus.Errorf("err %v\n", err)
        return err
    }
    d.driver = driver
    d.home = path.Join(home, "dfs")
    d.options = options
    logrus.Infof("Init - basedir %s\n", d.home)
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
func (d *Driver) Create(id, parent string) error {
    logrus.Infof("Create - id %s parent %s\n", id, parent)
    return d.ioctl(SNAP_CREATE, parent, id)
}

// CreateReadWrite creates a layer that is writable for use as a container
// file system.
func (d *Driver) CreateReadWrite(id, parent string) error {
    logrus.Infof("CreateReadWrite - id %s parent %s\n", id, parent)
    return d.ioctl(CLONE_CREATE, parent, id)
}

// Remove the filesystem with given id.
func (d *Driver) Remove(id string) error {
    logrus.Infof("Remove - id %s \n", id)
    return d.ioctl(SNAP_REMOVE, "", id)
}

// Get the requested filesystem id.
func (d *Driver) Get(id, mountLabel string) (string, error) {
    logrus.Infof("Get - id %s mountLabel %s\n", id, mountLabel)
    dir := path.Join(d.home, id)
    err := d.ioctl(SNAP_MOUNT, "", id)
    if err != nil {
        logrus.Errorf("err %v\n", err)
        return "", err
    }
    logrus.Infof("Get returning dir %s\n", dir)
    return dir, nil
}

// Put is kind of unmounting the file system.
func (d *Driver) Put(id string) error {
    logrus.Infof("Put - id %s \n", id)
    return d.ioctl(SNAP_UMOUNT, "", id)
}

func (d *Driver) Exists(id string) bool {
    logrus.Infof("Exists - id %s \n", id)
    err := d.ioctl(SNAP_STAT, "", id)
    return err == nil
}

// Status returns current driver information in a two dimensional string array.
// Output contains "Build Version" and "Library Version" of the dfs libraries used.
// Version information can be used to check compatibility with your kernel.
func (d *Driver) Status() [][2]string {
    logrus.Infof("Status \n")
    return [][2]string{[2]string{"Build Version", "1.0"},
                       [2]string{"Library Version", "1.0"}}
}

// GetMetadata returns empty metadata for this driver.
func (d *Driver) GetMetadata(id string) (map[string]string, error) {
    logrus.Infof("GetMetadata - id %s \n", id)
    return nil, nil
}

// Cleanup unmounts the home directory.
func (d *Driver) Cleanup() error {
    logrus.Infof("Cleanup \n")
    return d.ioctl(UMOUNT_ALL, "", "")
}

func (d *Driver) Diff(id, parent string) io.ReadCloser {
    logrus.Infof("Diff - id %s parent %s\n", id, parent)
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

func (d *Driver) Changes(id, parent string) ([]graphPlugin.Change, error) {
    logrus.Infof("Changes - id %s parent %s\n", id, parent)
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

// XXX This is somehow broken with docker
func (d *Driver) ApplyDiff(id, parent string, archive io.Reader) (int64, error) {
    logrus.Infof("ApplyDiff - id %s parent %s archive %+v\n", id, parent, archive)
    return d.driver.ApplyDiff(id, parent, archive)
}

func (d *Driver) DiffSize(id, parent string) (int64, error) {
    logrus.Infof("DiffSize - id %s parent %s\n", id, parent)
    return d.driver.DiffSize(id, parent)
}

func main() {
    logrus.Infof("Starting to serve requests\n")
    handler := graphPlugin.NewHandler(&Driver{driver: nil, init: Init,
                                      home: "", options: nil})
    //if err := handler.ServeUnix("root", "pxg"); err != nil {
    if err := handler.ServeTCP("pxg", ":32456", nil); err != nil {
        log.Fatalf("Error %v", err)
    }
    for {
    }
}
