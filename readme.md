# Kronos Compiler Suite #

## Vesa Norilo - University of Arts ##

This is a mirror of the original repository, hosted on Sourcehut: https://hg.sr.ht/~vnorilo/kronos.

In this Github repository some minor fixes are pushed to make building Kronos easier.

Please visit the main web page of the project at [kronoslang.io](https://kronoslang.io)

### License ###

All the source code in this repository is licensed under [GPL3](http://www.gnu.org/copyleft/gpl.html). This includes the runtime library, and by proxy, automatically imposes GPL3 on any of your custom software that uses the supplied library. Please contact the repository owner if you are interested in a different licensing model.

### Welcome to Kronos ###

Kronos is a programming language and a compiler for musical signal processing. This is the initial release of source code and binaries. As of yet, there is not much documentation. The distribution comes with a test suite with some example programs. The stable capabilities of the compiler are those covered by the test suite.

### Installing from binary ###

You can install one of the prebuilt packages targeted to Windows 10+, macOS, or Linux. These are available as releases in the Github release page.

### Compiling from Source ###

The dependencies for compiling Kronos are:

    1. cmake
    2. subversion

These can be retrieved differently according to the OS:

#### Windows ####

You may need run Powershell as admin. The easiest way to get all the dependencies is to install the Chocolatey package provider and follow the instructions below. Alternatively, you can install CMake and Subversion manually.

```
Install-PackageProvider ChocolateyGet
```

Windows may prompt you about installing `nuget` and trusting `ChocolateyGet` and downloading 'Choco'.

```
choco install cmake,svn
```

Prior to Windows 10, you can install [chocolatey](https://chocolatey.org/) and install the above packages via `choco install`.

#### MacOS ####

Get Xcode from the app store. Then you can install the dependencies via [Homebrew](https://brew.sh/).

```
brew install cmake subversion
```

#### Ubuntu 20.04 LTS ####

```
sudo apt-get install g++ python cmake libjack-dev libsndfile-dev libtinyxml-dev libreadline-dev libcurl4-openssl-dev
```

### Building ###

The easiest way to build Kronos is by using the `build.sh` script included in the repository. This will take care of building both LLVM and Kronos. The results will be stored in the `kronos` folder.

### Getting Started ###

A good way to get started is to run the test suite via *ktests*. It runs a set of Kronos programs via *krepl*. The tests include both expression evaluation against an expected result, and audio signal synthesis where a small relative error is tolerated due to floating point considerations.

If you want to see the command lines *ktests* uses, run it with the *--verbose* switch.

I'm working on adding introductory material to the [Wiki][].
[Wiki]:https://bitbucket.org/vnorilo/k3/wiki/Home