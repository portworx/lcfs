# LCFS Storage Driver Roadmap
The LCFS storage driver is meant to improve the full experience with containers from building, to deploying, and managing containers in production. This roadmap serves to communicate the project’s direction, is open to feedback, and is subject to change. 

The next three releases will focus on specific portions of the container lifecycle as identified below. Please [contact us](https://groups.google.com/forum/#!forum/portworx) with questions, [file issues](https://github.com/portworx/lcfs/issues), and contribute!

## Phase 1: Build and CI/CD with Containers 
Release: 0.4
Target Date: mid-April

Recently, LCFS improved its algorithm to track and commit changes. The result is that commits operations finish in constant time speeding up build times, irrespective of the size of the layer and  amount of changes. As LCFS continues to speed up container build operations, the experience with setup and use of LCFS itself should also improve. 

In this next release of LCFS, the following enhancements are planned:
* **mac support**: commonly used for build and initial testing, adding Mac support is just goodness.
* **Build pipeline**: adding setup examples for how to install LCFS in Jenkins, Travis, and/or other development environments.
* **OOBE**: sets of changes to make the overall setup smoother across all distros, especially Ubuntu, Debian, and CentOs. 

## Phase 2: Statistics and Space Management
Release: 0.5
Target Date: TBD

This release will enrich the stats and metadata that users and administrators can query from LCFS. Examples include what are the top layers, hot files, and reporting startup times by images. These stats should be consumable by users and common monitoring tools. Therefore, the stats and metadata will be exposed over a standard command line and REST interface. 

As a second pillar, LCFS will focus on improving space management. This release will include a garbage collection feature that will automatically remove unused images.  Image removal will be within a specifiable configuration. 

Examples of what should be easily queryable: 
* **Images**: what are the top 100 container images.
* **Layers**: top 100 layers and the corresponding image it belongs to.
* **Stats** for
  * **Time**: sort above by duration of startup, pull, and other lifecycle event. 
  * **Uptime**: time since (most recent) launch of container.    

## Phase 3: Distributed Control [Part 1]
Release: 0.6
Target Date: TBD

Containers are deployed in multi-server environments. Many of the container image operations today place heavy load on resources (e.g. pull loads registries) and require maintenance. To increase the automatability and control for users, LCFS will start to add multi-machine operations.

The first multi-machine (or distributed) operation will be reducing the load of pulls. Today, every single machine might pull against a registry. A basic heuristic will be added to understand which servers might cooperatively satisfy that pull. The net effect should be reduced load.

Additionally, the statistics and metadata enhancements in the prior theme will be upgraded with distributed control points. The result will be that a cluster of servers can then be quickly queried. In that cluster, a few commands can then report back the top images, layers, files, and stats as opposed to manually probing each server.

In later milestones, we will add more controls, enhance stats, and seek to improve the experience and automation with container images. 
