# lsd_ceph
Log-structured virtual disk in Ceph

For instructions on building and deploying this project, check out the BUILDME.md file.

Source code can be found at ![this](https://github.com/SiddheshRane/librbdbs3) repository.

## 1. Vision and Goals of the Project

 -  Implement the basic librbd API to work with the research block device
 -  Show that QEMU/KVM work with this library

## 2. Users/Personas Of The Project

This project is designed for a researcher to integrate their next-generation block device to a RBD API, instead of solely a Linux kernel API. It also could be usable for other researchers who wish to test their block device using virtualized deployments, instead of the Linux kernel.

## 3. Scope and Features Of The Project:

  Here are the feature goals for the project:
  
  - Expose a C-Compatible API for integrating a block device with the subset of librbd
  - Use QEMU/KVM to operate with the research block device, through this library
  
 
  Here are the goals we are unsure about:
  
   - Additional tools for image creation, etc.
   - Demonstration of using the research virtual disk via QEMU/KVM in the OpenStack framework
    - Demonstration of Kubernetes PVCs based on research disk and rbd-nbd
   - Running existing RBD test suites over research disk
    - Modifications and improvements to the research disk
    -  Write test cases for block 
  
  Here are the non-goals for the project:
  
   - We will not be writing our own block device from scratch, instead connecting to the existing research one
   
### 3.5 Lingering Questions about the Project

  - What does the functionality of LD_PRELOAD bring to the project, instead of directly integrating with librbd?
  - Are we looking to build middleware between the research block device and librbd, or are we modifying the research block device?
  - How can we test the functionality of the research block device when working with QEMU and Kubernetes?
  - How do we build and run the existing code? Do we have to build our own kernel?

## 4 Solution concept

 We will be using Go to either modify or extend the research block device in order to allow functionality between it and the API for Ceph's librbd.
 
 Based on the lingering questions, we will understand more about the different procedures for testing to ensure that the library is performing as expected, and we will also understand how to run test suites and integrate our code with Kubernetes and OpenStack.
 
 Currently, our rough idea looks as follows:
 ![6620](https://user-images.githubusercontent.com/74415990/134605606-0dfd21b2-eaa9-4efd-b1c6-d8b88bd33544.png)

## 5. Acceptance criteria

Minimum acceptance criteria is a library that allows for the block device to be written to and read from, in a QEMU/KVM virtualized environment.

Stretch goals include passing existing test suites for RBD, improving upon the research disk, and supporting Kubernets PVCs.

## 6. Release planning
 We will be splitting our time across 5 2-week long sprints. Below is the functionality that we hope to achieve by the end of the sprint.
 
 - Sprint 1: Understand lingering questions, construct a minimal example of the Go - C compatibility, determine which rbd functions to implement.
 - Sprint 2: Begin implementing library and writing unit tests.
 - Sprint 3: Finish crafting the API and test integration of research block device with QEMU/KVM in a virtualized environment.
 - Sprint 4: Integrate Kubernetes PVCs, and also run and pass existing RBD test suites.
 - Sprint 5: Allow for portable RBD image creation. (???)

## General Notes

Ceph is a scale-out object storage system, that allows for 3 interaction points. We will focus on Ceph RBD, which behaves as a block device (disk). We will use the existing librbd(CEPH's project) to connect our research block device (which currently has a Linux kernel interface) to Ceph, which in turn, through rbd-nbd and qemu-rbd, allow us to interact with Kubernetes PVC's and OpenStack via QEMU/KVM. Although librbd's API is long, we will select only the relevant functions to expose via the Go Language and it's C-Compatible API tooling. We estimate, conservatively, about 15 API calls to implement. You can find the relevant research projects [here](https://github.com/asch/dis), [here](https://github.com/asch/bs3), and [here](https://github.com/asch/buse).

## Build and Test Guide
[Build and Test Guide](/build.md)

## Demos

### Final Demo - December 8, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/wpOsMVFjCA0/0.jpg)](https://youtu.be/wpOsMVFjCA0 "Log structured virtual disk Final Project")
### Demo 5 - December 6, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/kH5t9IzIbu4/0.jpg)](https://youtu.be/kH5t9IzIbu4 "Log structured virtual disk for Ceph NEU CS6620 Demo #5")
### Demo 4 - November 17, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/3aT3UE8M4LY/0.jpg)](https://youtu.be/3aT3UE8M4LY "Log structured virtual disk for Ceph NEU CS6620 Demo #4")
### Demo 3 - November 4, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/ECu2aU-uKKQ/0.jpg)](https://www.youtube.com/watch?v=ECu2aU-uKKQ "Log structured virtual disk for Ceph NEU CS6620 Demo #3")
### Demo 2 - October 21, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/2DXEPQgI0OM/0.jpg)](https://www.youtube.com/watch?v=2DXEPQgI0OM "Log structured virtual disk for Ceph NEU CS6620 Demo #2")

### Demo 1 - October 7, 2021
[![IMAGE_ALT_TEXT](http://img.youtube.com/vi/ycN05trcXdA/0.jpg)](https://www.youtube.com/watch?v=ycN05trcXdA "Log structured virtual disk for Ceph NEU CS6620 Demo #1")

### Demo 0.5 - September 27, 2021

[![IMAGE ALT TEXT](http://img.youtube.com/vi/-0fYlONZotE/0.jpg)](http://www.youtube.com/watch?v=-0fYlONZotE "Log structured virtual disk for Ceph NEU CS6620 Demo #0.5")
