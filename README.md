# libbfcp
Lib BFCP (fork from libbfcp)

An open source BFCP (Binary Floor Control Protocol, RFC4582) library written in C
and C++. Fork from the libbfcp written by Lorenzo Miniero (see Confiance open source project).
Repackaged and modified for IVeS solutions. We mostly added support for BFCP over UDP.

code sources:
* https://github.com/meetecho/libbfcp
* https://github.com/InteractiviteVideoEtSystemes/libbfcp


## Build instructions

The instructions below were tested on CentOS 6 64 bits.

0- quick test with samples
    
    $ cd BFCP && make -j8             # to build bfcp library
    $ cd libbfcp/samples && make -j8  # to build the sample code


1- Install prerequistes

    # yum install gcc-c++, make, rpm-build


2- Produce the RPM package

Do not compile as root but use a normal linux user

    $ git clone https://github.com/InteractiviteVideoEtSystemes/libbfcp.git
    $ cd libbfcp
    $ ./install.ksh rpm nosign

3- Install the RPM package

    $ su
    Password: xxxxx
    # rpm -ivh libbfcp-x.y.z-t.ives.el6.x86_64.rpm

Libraries are compiled in both debug and release mode. They are located in

    /opt/ives/lib64

Header files are located in 

    /opt/ives/include

## materials
* https://github.com/embeddedmz/socket-cpp/tree/master

