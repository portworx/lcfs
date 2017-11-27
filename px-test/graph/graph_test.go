package graph

import (
	"fmt"
	log "github.com/Sirupsen/logrus"
	"testing"
)

func TestStorageDriveChange(t *testing.T) {

	var err error
	g := &Graph{
		Name:   "devicemapper",
		Driver: "devicemapper",
	}
	err = g.StartWithGraphDriver()
	if err != nil {
		log.Fatal(err)
	}

}

func TestRunConcurrent(t *testing.T) {
	g := &Graph{
		Name:   "devicemapper",
		Driver: "devicemapper",
	}
	_ = g.StartWithGraphDriver()
	_, elapsed := g.RunMultiple(4, "fedora/apache")
	fmt.Println("Elapsed time is :", elapsed)
	//hstat := &hoststat{}
	hstat := g.GatherHostStats("/dev/xvdg")
	fmt.Println("Inode :", hstat.InodeUsage)
	fmt.Println("Disk Usage :", hstat.DiskUsage)
	fmt.Println("Cpu :", hstat.Cpu)
	fmt.Println("BwRead :", hstat.LibDisk.BwRead)
	fmt.Println("BwWrite :", hstat.LibDisk.BwWrite)
}
