sloc)  2.05 KB
package main

import (
	"fmt"
	"github.com/codegangsta/cli"
	"github.com/portworx/px-test/graph"
	"os"
)

func runTests(c *cli.Context) {
	fmt.Println("run ")
	//fn := "run"
	g := &graph.Graph{
		Name:   "devicemapper",
		Driver: "devicemapper",
	}

	testName := c.String("test")
	driverName := c.String("driver")
	libDevice := c.String("libDevice")
	fmt.Println("Test Name:\n", testName)
	switch testName {
	case "Scalability":
		g.TestScalability(driverName, 20)
	case "PageCacheUsage":
		g.TestPageCacheUsage(driverName, libDevice)
	case "SpeedOfPull":
		g.TestPullSpeed(driverName)
	case "BuildSpeed":
		g.TestBuildSpeed(driverName)
	case "Stability":
		g.TestStability(driverName, 20)
	}
}

func main() {
	app := cli.NewApp()
	app.Name = "graphctl"
	app.Usage = "Portworx Automated Graph Benchmarking Test"
	app.Version = "0.1"
	app.Flags = []cli.Flag{
		cli.IntFlag{
			Name:  "log-level,l",
			Usage: "Set the logging level",
			Value: 0,
		},
		cli.IntFlag{
			Name:  "priority,p",
			Usage: "Run tests with specified priority",
			Value: 0,
		},
	}
         
    // XXX Specifying All argument for tests/driver does not seem to work.
	app.Commands = []cli.Command{
		{
			Name:    "run",
			Aliases: []string{"r"},
			Usage:   "Run specified tests for the specified graph driver\n\t Supported Tests: Scalability,PageCacheUsage,SpeedOfPull,BuildSpeed,Stability or All \n\t eg: ./graphctl run --test Scalability --driver overlay --lib-device=<device> | ./graphctl run --test All --driver All",
			Action:  runTests,
			Flags: []cli.Flag{
				cli.StringFlag{
					Name:  "test,t",
					Usage: "Run specified tests for the specified graph driver\n\t Supported Tests: Scalability,PageCacheUsage,SpeedOfPull,BuildSpeed,Stability or All",
					Value: "All",
				},
				cli.StringFlag{
					Name:  "driver,d",
					Usage: "Graph Driver to run tests against \n\t Supported Drivers: overlay, devicemapper, osd",
					Value: "All",
				},
				cli.StringFlag{
					Name:  "lib-device,lib",
					Usage: "Lib device for specified driver",
					Value: "/dev/xvdg",
				},
			},
		},
	}
	app.Run(os.Args)
}
