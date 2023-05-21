# Kronos Compiler Suite #

## Vesa Norilo - University of Arts Helsinki ##

This is a mirror of the original repository, hosted on Sourcehut: https://hg.sr.ht/~vnorilo/kronos.

This Github repository contains some minor fixes to make building Kronos easier.

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
choco install cmake svn
```

#### MacOS ####

Get Xcode from the app store. Then you can install the dependencies via [Homebrew](https://brew.sh/).

```
brew install cmake subversion
```

#### Ubuntu ####

```
sudo apt-get install g++ python cmake libjack-dev libsndfile-dev libreadline-dev libcurl4-openssl-dev
```

### Building ###

The easiest way to build Kronos is by using the `build.sh` script included in the repository. This will take care of building both LLVM and Kronos. The results will be stored in the `kronos` folder.

### Getting Started ###

To get started with Kronos, visit the Resources page on the website: https://kronoslang.io/resources.

