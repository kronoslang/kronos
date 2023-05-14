# Kronos Compiler Suite #
## Vesa Norilo ##
## University of Arts ##

Please visit the main web page of the project at [kronoslang.io](https://kronoslang.io)

### License ###

All the source code in this repository is licensed under [GPL3](http://www.gnu.org/copyleft/gpl.html). This includes the runtime library, and by proxy, automatically imposes GPL3 on any of your custom software that uses the supplied library. Please contact the repository owner if you are interested in a different licensing model.

### Welcome to Kronos ###

Kronos is a programming language and a compiler for musical signal processing. This is the initial release of source code and binaries. As of yet, there is not much documentation. The distribution comes with a test suite with some example programs. The stable capabilities of the compiler are those covered by the test suite. 

### Installing from binary ###

You can install one of the prebuilt packages targeted to Windows 10+, macOS, or Debian. Windows and Mac installers are componentized so you may choose whether to install developer headers and the test suite, for example. 

See the next section for details about the runtime library placement.

### Getting Started 

A good way to get started is to run the test suite via *ktests*. It runs a set of Kronos programs via *krepl*. The tests include both expression evaluation against an expected result, and audio signal synthesis where a small relative error is tolerated due to floating point considerations.

If you want to see the command lines *ktests* uses, run it with the *--verbose* switch.

I'm working on adding introductory material to the [Wiki][].
[Wiki]:https://bitbucket.org/vnorilo/k3/wiki/Home

### Compiling from Source ###

The dependencies for compiling Kronos are:

#### Windows ####

You may need run Powershell as admin. The easiest way to get all the dependencies is to install the Chocolatey package provider and follow the instructions below. Alternatively, you can install Visual Studio 2017, CMake, Mercurial, Subversion and Python 2.7 manually.

```
Install-PackageProvider ChocolateyGet
```

Windows may prompt you about installing `nuget` and trusting `ChocolateyGet` and downloading 'Choco'.

```
Install-Package VisualStudio2017Community,cmake,hg,svn
Install-Package python -RequiredVersion 2.7.11
```

Prior to Windows 10, you can install [chocolatey](https://chocolatey.org/) and install the above packages via `choco install`.

#### MacOS ####

Get Xcode from the app store. Then you can install the dependencies via [Homebrew](https://brew.sh/).

```
brew install cmake mercurial subversion
```

#### Ubuntu 20.04 LTS ####

```
sudo apt-get install g++ python cmake mercurial libjack-dev libsndfile-dev libtinyxml-dev libreadline-dev libcurl4-openssl-dev
# optionally install system llvm if you don't want to build it
sudo apt-get install llvm-6.0-dev
```

### Configuration ###

The easiest way to get Kronos configured, especially if you will build LLVM yourself, is to use the [buildbot script](https://bitbucket.org/vnorilo/k3bot). By default, the bot performs continuous integration and delivery. You can skip the integration and just build locally by using:

```
cd <workspace>
hg clone https://bitbucket.org/vnorilo/k3bot
cmake k3bot -DUSE_BUCKETBOT=False -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

This will download, configure and compile LLVM and Kronos. Alternatively you can install a precompiled LLVM 6.x and add the configuration option -DUSE_SYSTEM_LLVM=True

If k3bot configuration succeeds but you have problems building the tip of the repository, which is not necessarily stable, please try the latest release instead.

```
cd Source/kronos
hg tags
hg up <some-tag>
cd ../../Build/kronos
make
```

You can build the install target to put the resulting binaries in your system path. On Unix-like systems the binaries are put in either /usr or /usr/local, split into bin and lib. The runtime library is put in /share/kronos/Lib under the prefix. On Windows, the prefix will be an user-specified location in Program Files. 

The test suite includes a bunch of statically compiled binaries via *kc*. To run them, use *make test* or the related IDE build target.
