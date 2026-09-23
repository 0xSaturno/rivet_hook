# SPDX-FileCopyrightText: 2025-2026 Neptuwunium
#
# SPDX-License-Identifier: EUPL-1.2

project('rivet_hook', 'cpp',
	license: 'EUPL-1.2',
	version: '1.1.1',
	default_options: [
		'prefix=/usr',
		'cpp_std=c++20',
        'default_library=static',
	],
    meson_version: '>=1.2.1')

compiler = meson.get_compiler('cpp')

cpp_args = []

cpp_args = compiler.get_supported_arguments(
    '/wd4061',
    '/wd4068',
    '/wd4070',
    '/wd4201',
    '/wd4365',
    '/wd4464',
    '/wd4582',
    '/wd4583',
    '/wd4623',
    '/wd4625',
    '/wd4626',
    '/wd4668',
    '/wd4710',
    '/wd4711',
    '/wd4820',
    '/wd4866',
    '/wd5027',
    '/wd5039',
    '/wd5045',
    '/wd5267',
    '/utf-8',
    '/EHsc',
    '-fPIC',
    '-finput-charset=utf-8',
    '-fexec-charset=utf-8',
    '-faddrsig',
    '-faligned-new',
    '-Wno-unknown-pragmas',
    '-Wall',
    '-Wextra',
    '-Wno-pedantic',
    '-Wno-unused-but-set-variable',
    '-Wno-unused-variable',
    '-Wno-unused-parameter',
    '-Wno-deprecated-literal-operator',
)

minhook_dep = dependency('minhook', version: '>= 1.3.3')
minizip_dep = dependency('minizip-ng', version: '>= 4.0.0', required: false)
if not minizip_dep.found()
    minizip_dep = subproject('minizip-ng', default_options: {
        'zlib': 'enabled'
    }, required: true).get_variable('minizip_ng_dep')
endif

nlohmann_json_dep = dependency('', required: false)
if not compiler.check_header('nlohmann/json.hpp')
    nlohmann_json_dep = dependency('nlohmann_json', version: '>= 3.11.2')
endif

toml11_dep = dependency('', required: false)
if not compiler.check_header('toml.hpp')
    toml11_dep = dependency('toml11', version: '>= 4.4.0')
endif

# imgui has no stable api, can't rely on system headers, we also are the only one disabling obsolete functions
imgui_dep = subproject('imgui-docking',
   default_options: {
       'dx12': 'enabled',
       'win': 'enabled',
       'disable_obsolete_functions': true,
   },
   required: true).get_variable('imgui_dep')

dstorage_dep = dependency('directstorage', version: '= 1.2.2')
dxgi_dep = compiler.find_library('dxgi', required: true)
d3d12_dep = compiler.find_library('d3d12', required: true)
directxmath_dep = dependency('directxmath', version: '>= 2024.10.15.1')
threads_dep = dependency('threads')

subproject('hid_hook', default_options: {
    'hook_name': meson.project_name()
})

sources = [
    'src/main.cpp',
    'src/runtime.cpp',
    'src/runtime_loader.cpp',
    'src/settings.cpp',
    'src/signature_engine.cpp',
    'src/vk_enum.cpp',
]

deps = [
    minhook_dep,
    minizip_dep,
    nlohmann_json_dep,
    toml11_dep,
    dstorage_dep,
    threads_dep,
]

if not get_option('loader_only')
    # the interpreter and luac are dead weight inside a dll, and loadlib would let
    # a script pull an arbitrary library into the game's address space
    lua_dep = subproject('lua', default_options: {
        'interpreter': false,
        'loadlib': 'disabled',
        'line_editing': 'disabled',
    }, required: true).get_variable('lua_dep')

    sources += [
        'src/bridge.cpp',
        'src/camera.cpp',
        'src/configs.cpp',
        'src/ddl.cpp',
        'src/ddl_visit.cpp',
        'src/ddl_inspector.cpp',
        'src/events.cpp',
        'src/game_thread.cpp',
        'src/hud.cpp',
        'src/scene_query.cpp',
        'src/time_scale.cpp',
        'src/vanity.cpp',
        'src/watch.cpp',
        'src/overlay.cpp',
        'src/overlay_core.cpp',
        'src/scripting.cpp',
        'src/script_signal.cpp',
    ]
    deps += [
        imgui_dep,
        dxgi_dep,
        d3d12_dep,
        directxmath_dep,
        lua_dep,
    ]
else
    cpp_args += ['-DRIVET_ONLY_LOADER']
endif

shared_library(meson.project_name(), sources,
    link_args: compiler.get_supported_arguments('-static-libgcc', '-static-libstdc++'),
    dependencies: deps,
    name_prefix: '',
    cpp_args: cpp_args
)
