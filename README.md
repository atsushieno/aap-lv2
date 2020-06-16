# AAP-LV2: LV2 backend and ports to AAP

This repository will be used to move LV2 specific stuff from [android-audio-plugin-framework](https://github.com/atsushieno/android-audio-plugin-framework).

Right now this repository is supposed to be checked out in the same directory as `android-audio-plugin-framework` (name fixed).

## Importing LV2 plugins


### directory structure conversion

LV2 packaging is not straightforward. Android native libraries are usually packaged like

- `lib/armeabi-v7a/libfoo.so`
- `lib/arm64-v8a/libfoo.so`
- `lib/x86/libfoo.so`
- `lib/x86_64/libfoo.so`

while normal `lv2` packages usually look like:

- `lib/foo.lv2/manifest.ttl`
- `lib/foo.lv2/foo.ttl`
- `lib/foo.lv2/foo.so`

Attempt to copy those `lv2` contents under `lib/{abi}` with simple build.gradle script failed. Asking plugin developers to add `copy(from/into)` operation hack (which might still not work) is awkward, so we would rather provide simpler solution - we put `lv2/` contents under `assets`, and put ABI-specific `*.so` files directly under `lib/{abi}`. Loading `*.so` from `assets` subdirectories is not possible either.

- `assets/foo.lv2/manifest.ttl`
- `assets/foo.lv2/foo.ttl`
- `lib/{abi}/foo.so`

The `import-lv2-deps.sh` does this task for `java/aaplv2samples` which is used by `aaphostsample` and `localpluginsample`.


### LV2_PATH workarounds

There is another big limitation on Android platform: it is not possible to get list of asset directories in Android, meaning that querying audio plugins based on the filesystem is not doable. All those plugins must be therefore explicitly listed at some manifest.

To address this issue, AAP-LV2 plugin service takes a list of LV2 asset paths from `aap_metadata.xml`, which are used for `LV2_PATH` settings. It is taken care by `org.androidaudioplugin.lv2.AudioPluginLV2ServiceExtension` and plugin developers shouldn't have to worry about it, as long as they add the following metadata within `<service>` for AudioPluginService:

```
            <meta-data
                android:name="org.androidaudioplugin.AudioPluginService#Extensions"
                android:value="org.androidaudioplugin.lv2.AudioPluginLV2ServiceExtension"
                />
```


### converting LV2 metadata to AAP metadata

We decided to NOT support shorthand metadata notation like

```
<plugin backend='LV2' assets='lv2/eg-amp.lv2' product='eg-amp.lv2' />
```

... because it will make metadata non-queryable to normal Android app developers.

Instead we provide a metadata generator tool `app-import-lv2-metadata` and ask LV2 plugin developers (importers) to describe everything in `aap-metadata.xml`:

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


### Licensing notice

Note that `mda-lv2` is distributed under the GPLv3 license and you have to
follow it when distributing or making changes to that part (the LV2 plugin
samples). This does not apply to other LV2 related bits.



## Build Dependencies

### Platform features and modules

TODO: move those descriptions to `android-native-audio-builders` repo.

android-audio-plugin-framework repo has some dependencies, which are either platform-level-specific, or external. Note that this is NOT about build script.

External software projects:

- lv2 category
  - libsndfile
    - libogg
    - libvorbis
    - flac
  - lilv (private fork)
    - serd (private fork)
    - sord (private fork)
    - sratom
  - cerbero (as the builder, private fork)
- vst3 category (TODO)
  - vst3sdk (no particular dependency found, for non-GUI parts)

There used to be cairo and all those dependencies, but they are all about "examples" in lv2 repo and totally optional. They are not really designed for Android, so now we skip them.

Also, unlike before, we don't build LV2 for desktop anymore. They are regarded as the installed packages on the system. Ubuntu 20.04 has those minimum requirement versions. If they don't exist for your distribution, build and install them from source.

### cerbero fork

The external dependencies are built using cerbero build system. Cerbero is a comprehensive build system that cares all standard Android ABIs and builds some complicated projects like glib (which has many dependencies) and cairo.

### LV2 forks

There are couple of lv2 related source repositories, namely serd and lilv. Their common assumption is that they have access to local files, which is not true about Android. They are forked from the original sources and partly rewritten to get working on Android.

And note that access to assets is not as simple as that to filesystem. It is impossible to enumerate assets at runtime. They have to be explicitly named and given. Therefore there are some plugin loader changes in our lilv fork.
