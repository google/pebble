# Tintin Native SDK Generator
This script exports white-listed functions, `typedef`s and `#define`s in tintin source tree so that they can be used by native-watch apps.

>
> ### Note:
>
> It is *not* possible to add additional publicly exposed functions to
> an already released firmware/SDK combination.
>
> This is because the generated `src/fw/pebble.auto.c` file needs
> to have been compiled into the firmware with which the SDK will be
> used.
>
> If you just expose a new function in `exported_symbols.json`,
> generate a new SDK and compile an app that uses the new function
> then the watch will crash when that code is executed on a firmware
> without that function exposed.
>
> You will need to generate and release a new firmware alongside the
> new SDK build that has the newly exposed function.
>

The script generates 3 files required to build native watchapps:
+ `sdk/include/pebble.h` -- Header file containing the typedefs, defines and function prototypes for normal apps
+ `sdk/include/pebble_worker.h` -- Header file containing the typedefs, defines and function prototypes for workers
+ `sdk/lib/pebble.a` -- Static library containing trampolines to call the exported functions in Flash.
+ `src/fw/pebble.auto.c` -- C file containing `g_pbl_system_table`, a table of function pointers used by the trampolines to find an exported function's address in Flash.

## Running the generator
The running of the generator has now been integrated into the build, so there is no need to run it separately.

If for whatever reason, you need to run the generator by hand, run `% tools/generate_native_sdk/generate_pebble_native_sdk_files.py --help`, and it's simple enough to follow.


## Configuration
The symbols exported by the SDK generator are defined in the `exported_symbols.json` config file.

The format of the config file is as follows:

    [
        {
            "revision" : "<exported symbols revision number>",
            "version" : "x.x",
            "files" : [
                <Files to parse/search>
             ],
            "exports" : [
                <Symbols to export>
            ]
        }
    ]

Each exported symbol in the `exports` table is formatted as follows:

    {
        "type" : "<Export type",
        "name" : "<Symbol name>",
        "sortName" : "<sort order>",
        "addedRevision" : "<Revision number>"
    }

`Export type` type can be any of `function`, `define`, `type`, or `group`. A `group` type has the following structure:

    {
        "type : "group",
        "name" : "<Group Name>",
        "exports" : [
          <Symbols to export>
        ]
    }

*NB:* The generator sorts the order of the `functions` in order of addedRevision, and then alphabetically within a revision using sortName (if specified) or name. The `types` fields are left in the order in which they are entered in the types table. As well, Be sure to specify any typedefs with dependencies on other typedefs after their dependencies in the `types` list.

### Important!
When adding new functions, make sure to bump up the `revision` field, and use that value as the new functions' `addedRevision` field. This guarantees that new versions of TintinOS are backwards compatible when compiled against older `libpebble.a`. Seriously, ***make sure to do this***!!.

## Bugs
+ The script doesn't check the resulting `pebble.h` file will compile, that is left as an exercise to the reader.
+ The script's error reporting is a little funky/unfriendly in places
+ The script does not do any checking of the function revision numbers, beyond a simple check that the file's revision is not lower than any function's.
