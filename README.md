# lsd_ceph
Log-structured virtual disk in Ceph


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
