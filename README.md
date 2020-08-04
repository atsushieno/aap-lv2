# AAP-LV2: LV2 backend and ports to AAP

This repository contains LV2 backend (or wrapper, or whatever) support for [android-audio-plugin-framework](https://github.com/atsushieno/android-audio-plugin-framework), as well as samples.

The sample ports include:

- https://gitlab.com/drobilla/mda-lv2
- https://github.com/brummer10/guitarix
- https://github.com/sfztools/sfizz (still needs some host that can pass sfz filename)


## Building

Right now this repository is supposed to be checked out in the same directory as `android-audio-plugin-framework` (name fixed).

`make` should take care of the builds.


## Limitations

LV2 Dynamic Manifest feature expects that the hosting environment has access to writable temporary directory. It is wrong assumption and LV2 specification has to remove any reference to `FILE*` in the public API. It is not supported in AAP.

LV2 State `makePath` feature is problematic for the same reason.


## Importing LV2 plugins

There are couple of steps to import existing LV2 plugsin into AAP world:

- Build plugin binaries for Android ABIs
- Convert LV2 resources to Android assets
- Generate `aap_metadata.xml` (or create it manually)

### building plugin binaries for Android ABIs

The dependency Android binaries are built from [android-native-audio-builders](https://github.com/atsushieno/android-native-audio-builders) repository. It builds LV2/lilv dependencies as well as plugin sample binaries.

For LV2, mda-lv2, and guitarix, the `make` step lets it download from the release tarballs.

### directory structure conversion

AAP-LV2 packaging is not straightforward, because the file layouts is differrent from that on Linux. Android native libraries are usually packaged like:

- `lib/armeabi-v7a/libfoo.so`
- `lib/arm64-v8a/libfoo.so`
- `lib/x86/libfoo.so`
- `lib/x86_64/libfoo.so`

while normal `lv2` packages usually look like:

- `lib/foo.lv2/manifest.ttl`
- `lib/foo.lv2/foo.ttl`
- `lib/foo.lv2/foo.so`

Those `lib/*.lv2/*.so` files cannot be dynamically loaded unlike Linux desktop, so they have to be moved to `lib/*/` directory. Other LV2 manifests are packaged under `assets/lv2` directory. Therefore, the file layout is:

- `assets/foo.lv2/manifest.ttl`
- `assets/foo.lv2/foo.ttl`
- `lib/{abi}/foo.so`

The `import-lv2-deps.sh` does this task for `aap-mda-lv2`. Similarly, `import-guitarix-deps.sh` does it for `aap-guitarix`.

### AAP Metadata

AAP needs `aap_metadata.xml` in `res/xml`. It can be generated from LV2 manifests in the plugin directory, using `aap-import-lv2-metadata` tool:

```
$ ./tools/aap-import-lv2-metadata/aap-import-lv2-metadata [lv2path] [res_xml_path]
```

The way how this tool generates metadata from LV2 manifests is described in depth later.


## Implementation details

### LV2_PATH workarounds

There is a big limitation on Android platform: it is not possible to get list of asset directories in Android, meaning that querying audio plugins based on the filesystem is not doable. All those plugins must be therefore explicitly listed at some manifest.

To address this issue, AAP-LV2 plugin service takes a list of LV2 asset paths from `aap_metadata.xml`, which are used for `LV2_PATH` settings. It is taken care by `org.androidaudioplugin.lv2.AudioPluginLV2ServiceExtension` and plugin developers shouldn't have to worry about it, as long as they add the following metadata within `<service>` for AudioPluginService:

```
            <meta-data
                android:name="org.androidaudioplugin.AudioPluginService#Extensions"
                android:value="org.androidaudioplugin.lv2.AudioPluginLV2ServiceExtension"
                />
```


### converting LV2 metadata to AAP metadata

We provide a metadata generator tool `app-import-lv2-metadata` and ask LV2 plugin developers (importers) to describe everything in `aap-metadata.xml`.

```
$ ./aap-import-lv2-metadata [lv2path] [res_xml_path]
(...)
LV2 directory: /sources/android-audio-plugin-framework/java/samples/aaphostsample/src/main/assets/lv2
Loading from /sources/android-audio-plugin-framework/java/samples/aaphostsample/src/main/assets/lv2/ui.lv2/manifest.ttl
Loaded bundle. Dumping all plugins from there.
all plugins loaded
Writing metadata file java/samples/aaphostsample/src/main/res/xml/aap_metadata.xml
done.

$ cat res/xml/metadata0.xml 
<plugins>
  <plugin backend="LV2" name="Example MIDI Gate" category="Effect" author="" manufacturer="http://lv2plug.in/ns/lv2" unique-id="lv2:http://lv2plug.in/plugins/eg-midigate" library="..." entrypoint="...">
    <ports>
      <port direction="input" content="midi" name="Control" />
      <port direction="input" content="audio" name="In" />
      <port direction="output" content="audio" name="Out" />
    </ports>
  </plugin>
</plugins>
$ cat manifest-fragment.xml 
<plugins>
  <plugin backend="LV2" name="MDA Ambience" category="Effect" author="David Robillard" manufacturer="http://drobilla.net/plugins/mda/" unique-id="lv2:http://drobilla.net/plugins/mda/Ambience" library="libandroidaudioplugin-lv2.so" entrypoint="GetAndroidAudioPluginFactoryLV2Bridge" assets="/lv2/mda.lv2/">
    <ports>
      <port direction="input" default="0.700000" minimum="0.000000" maximum="1.000000" content="other" name="Size" />
      <port direction="input" default="0.700000" minimum="0.000000" maximum="1.000000" content="other" name="HF Damp" />
      <port direction="input" default="0.900000" minimum="0.000000" maximum="1.000000" content="other" name="Mix" />
      <port direction="input" default="0.500000" minimum="0.000000" maximum="1.000000" content="other" name="Output" />
      <port direction="input"    content="audio" name="Left In" />
      <port direction="input"    content="audio" name="Right In" />
      <port direction="output"    content="audio" name="Left Out" />
      <port direction="output"    content="audio" name="Right Out" />
    </ports>
  </plugin>
  ...
```

For `content`, if a port is `atom:atomPort` and `atom:supports` has `midi:MidiEvent`, then it is `midi`. Any LV2 port that is `lv2:AudioPort` are regarded as `audio`. Anything else is `other` in AAP.

The plugin `category` becomes `Instrument` if and only if it is `lv2:InstrumentPlugin`. Anything else falls back to `Effect`.

We don't detect any impedance mismatch between TTL and metadata XML; LV2 backend implementation uses "lilv" which only loads TTL. lilv doesn't assure port description correctness in TTL either (beyond what lv2validate does as a tool, not runtime).

(We decided to NOT support shorthand metadata notation like `<plugin backend='LV2' assets='lv2/eg-amp.lv2' product='eg-amp.lv2' />` in `aap_metadata.xml` because it will make metadata non-queryable to normal Android app developers. Also, we shouldn't need Service code running to just let it send back metadata to host only for querying. It should be self-explanatory.)


## Build Dependencies

### Platform features and modules

NOTE: the actual native library builds have moved to `android-native-audio-builders` repo.

android-audio-plugin-framework repo has some dependencies, which are either platform-level-specific, or external.

External software projects:

- lv2 dependencies
  - lilv (private fork)
    - serd (private fork)
    - sord (private fork)
    - sratom
- sfizz dependencies
  - cerbero (as the builder, private fork)
  - libsndfile
    - libogg
    - libvorbis
    - flac

- vst3 category (TODO)
  - vst3sdk (no particular dependency found, for non-GUI parts)

There used to be cairo and all those dependencies, but they are all about "examples" in lv2 repo and totally optional. They are not really designed for Android, so now we skip them.

Also, unlike before, we don't build LV2 for desktop anymore. They are regarded as the installed packages on the system. Ubuntu 20.04 has those minimum requirement versions. If they don't exist for your distribution, build and install them from source.

### cerbero fork

The external dependencies are built using cerbero build system. Cerbero is a comprehensive build system that cares all standard Android ABIs and builds some complicated projects like glib (which has many dependencies) and cairo.

### LV2 forks

There are couple of lv2 related source repositories, namely serd and lilv. Their common assumption is that they have access to local files, which is not true about Android. They are forked from the original sources and partly rewritten to get working on Android.

And note that access to assets is not as simple as that to filesystem. It is impossible to enumerate assets at runtime. They have to be explicitly named and given. Therefore there are some plugin loader changes in our lilv fork.


## Debugging with LV2 internals

Sometimes having binary-only LV2 SDK bits makes debugging difficult.
To improve such a situation, we can build all lilv and dependencies together
within libandroidaudioplugin-lv2.so. 
This is a patch @atsushieno created for making it possible: https://gist.github.com/atsushieno/969eedaeefb51d99309a3234c2f9b8de

To use it, you will have to:

- create a directory `lilv_direct` in `androidaudioplugin-lv2/src/main/cpp/src` and go there. In that directory...
  - `ln -s /path/to/android-native-audio-builders/serd serd` as well as `sord`, `sratom` and `lilv`.
  - `cp /path/to/android-native-audio-builders/build/x86/serd/build/serd_config.h .` as well as `sord/build/sord_config.h`, `sratom/build/sratom_config.h` and `lilv/build/lilv_config.h` (it may require exact ABI, I used `x86` here)
  - Put these additional sources to the directory.
    - `abstract_io.c` https://gist.github.com/atsushieno/ea113d7ce392fe31a65dfdda08e6a1b4
    - `abstract_io.h` https://gist.github.com/atsushieno/b9b6fd545d2eafbf94d6badf6cda961c
- go back to topdir, and `rm -rf androidaudioplugin-lv2/src/main/jniLibs/`

Also note that depending on the build approaches, the gradle build scripts may not reach the tasks for copying lv2 resources (e.g. `sfizz.lv2`). If they are missing in the apk, then it will fail to retrieve TTL files at instantiation time.


## Debugging with mda-lv2 internals

Similar to the previous section on lilv internals, it is also possible to build and debug mda-lv2 with sources. However, the required changes are different - especially in that it needs metadata changes.

- create symbolic from `android-native-audio-builders/mda-lv2` to (some path like) `aap-mda-lv2/src/mda_direct`
- add the sources to `CMakeLists.txt`, but you can build only one plugin because mda-lv2 iterates source builds in its waf land. `lvz/*.cpp` are required with any plugin.
- You will have to define some variables to compile sources. (see below)
- add `lvz` and `src` directories to `target_include_directories`.
- in `assets/lv2/mda-lv2/manifest.ttl`, find your target plugin to change and replace its `.so` file with `libaap-mda-lv2.so` (the one you are going to build). lilv will load the specified library.

The additional definitions you need:

```
        -DPLUGIN_CLASS=mdaEPiano
        -DURI_PREFIX="http://drobilla.net/plugins/mda/"
        -DPLUGIN_URI_SUFFIX="EPiano"
        -DPLUGIN_HEADER="mdaEPiano.h"
```

[An example patch](https://gist.github.com/atsushieno/a233bc7a527c02ef562b4151647ff698) which once worked is provided for reference (the directory structure is not strictly following the list above, but you would get the ideas).


## Performance measuring

`android-audio-plugin-lv2-bridge.cpp` has a simple performance measurement aid which can be enabled with JUCEAAP_LOG_PERF variable.

## Licensing notice

aap-lv2 codebase is distributed under the MIT license.

LV2 toolkit core parts (serd/sord/sratom/lilv/lv2) are under the ISC license.

There are some sources copied from [jalv](https://gitlab.com/drobilla/jalv) project mentioned in androidaudioplugin-lv2 sources, `symap.*`, as well as those files under `zix` directory, and they are distributed under the ISC license.

`guitarix` and `mda-lv2` are distributed under the GPLv3 license and you 
have to follow it when distributing or making changes to those parts.

`sfizz` is distributed under the MIT license, but it has dependencies that are under LGPL2.1, Apache License 2.0, and so on. See [the project repository](https://github.com/sfztools/sfizz) for details.

Each of those plugin sets is packaged into one application respectively.
