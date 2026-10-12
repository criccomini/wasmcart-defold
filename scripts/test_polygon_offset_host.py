#!/usr/bin/env python3
"""Exercise the production polygon-offset command with Lua and ASan/UBSan.

Requires a C++ compiler and a Lua 5.1-compatible pkg-config package (LuaJIT by
default). Compiles the actual binding, command constructors, queue insertion
and parser case from their source files. The render instance and graphics
backend are test doubles; this does not build an engine or rasterize polygons.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    if source.count(signature) != 1:
        raise ValueError("Expected one function: " + signature)
    start = source.index(signature)
    end = source.index("{", start)
    depth = 1
    while depth:
        end += 1
        depth += (source[end] == "{") - (source[end] == "}")
    return source[start:end + 1]


def main():
    script = (ROOT / "engine/render/src/render/render_script.cpp").read_text()
    command = (ROOT / "engine/render/src/render/render_command.cpp").read_text()
    header = (ROOT / "engine/render/src/render/render_command.h").read_text()
    declarations = header[header.index("    enum CommandType"):header.index("    void ParseCommands")]
    constructors = command[command.index("    Command::Command("):command.index("    void ParseCommands(")]
    polygon_case = command[command.index("                case COMMAND_TYPE_SET_POLYGON_OFFSET:"):command.index("                case COMMAND_TYPE_DRAW:")]
    harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>
extern "C" {
#include <lua.h>
#include <lauxlib.h>
}
namespace dmGraphics {
    typedef void* HContext;
    static float received_factor, received_units;
    static unsigned calls;
    static void SetPolygonOffset(HContext context, float factor, float units) {
        assert(context == &calls);
        received_factor = factor;
        received_units = units;
        ++calls;
    }
}
namespace dmRender {
DECLARATIONS
CONSTRUCTORS
    struct CommandBuffer {
        std::vector<Command> commands;
        bool Full() const { return commands.size() == Capacity(); }
        void Push(const Command& command) { commands.push_back(command); }
        unsigned Capacity() const { return 1; }
    };
    struct RenderScriptInstance { CommandBuffer m_CommandBuffer; };
    static RenderScriptInstance instance;
    static RenderScriptInstance* RenderScriptInstance_Check(lua_State*) { return &instance; }
INSERT_COMMAND
BINDING
    static void ParsePolygonOffset(Command* c) {
        dmGraphics::HContext context = &dmGraphics::calls;
        switch (c->m_Type) {
POLYGON_CASE
        default: assert(false);
        }
    }
}
static uint32_t Bits(float value) {
    uint32_t bits;
    static_assert(sizeof(bits) == sizeof(value), "32-bit float required");
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}
static int Call(lua_State* L, double factor, double units) {
    lua_pushcfunction(L, dmRender::RenderScript_SetPolygonOffset);
    lua_pushnumber(L, factor);
    lua_pushnumber(L, units);
    return lua_pcall(L, 2, 0, 0);
}
int main() {
    lua_State* L = luaL_newstate();
    assert(L);
    const double cases[][2] = {
        {0.125, 0.75}, {1.0, 2.0}, {0.125, -0.75}, {-1.25, -2.5}, {-1.0, -4.0},
        {0.0, -0.0}, {-0.0, 0.0}, {0.1, -0.2},
        {std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()},
        {std::numeric_limits<float>::min(), -std::numeric_limits<float>::min()},
        {std::numeric_limits<float>::denorm_min(), -std::numeric_limits<float>::denorm_min()},
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        dmRender::instance.m_CommandBuffer.commands.clear();
        assert(Call(L, cases[i][0], cases[i][1]) == 0);
        assert(dmRender::instance.m_CommandBuffer.commands.size() == 1);
        dmRender::Command* command = &dmRender::instance.m_CommandBuffer.commands[0];
        assert(command->m_Type == dmRender::COMMAND_TYPE_SET_POLYGON_OFFSET);
        float factor = (float) cases[i][0], units = (float) cases[i][1];
        unsigned before = dmGraphics::calls;
        dmRender::ParsePolygonOffset(command);
        assert(dmGraphics::calls == before + 1);
        assert(Bits(dmGraphics::received_factor) == Bits(factor));
        assert(Bits(dmGraphics::received_units) == Bits(units));
        assert(command->m_Operands[0] == Bits(factor));
        assert(command->m_Operands[1] == Bits(units));
    }
    // A full queue must report the existing Lua error without replacing a command.
    dmRender::Command saved = dmRender::instance.m_CommandBuffer.commands[0];
    assert(Call(L, 0.5, -0.5) != 0);
    assert(strstr(lua_tostring(L, -1), "Command buffer is full (1)."));
    lua_pop(L, 1);
    assert(dmRender::instance.m_CommandBuffer.commands.size() == 1);
    assert(dmRender::instance.m_CommandBuffer.commands[0].m_Operands[0] == saved.m_Operands[0]);
    assert(dmRender::instance.m_CommandBuffer.commands[0].m_Operands[1] == saved.m_Operands[1]);
    // Invalid arguments must not enqueue anything.
    dmRender::instance.m_CommandBuffer.commands.clear();
    lua_pushcfunction(L, dmRender::RenderScript_SetPolygonOffset);
    lua_pushboolean(L, 1);
    lua_pushnumber(L, 1.0);
    assert(lua_pcall(L, 2, 0, 0) != 0);
    lua_pop(L, 1);
    assert(dmRender::instance.m_CommandBuffer.commands.empty());
    lua_close(L);
    puts("polygon offset: 11 exact Lua/command/backend round trips and error paths passed");
}
'''
    replacements = {
        "DECLARATIONS": declarations,
        "CONSTRUCTORS": constructors,
        "INSERT_COMMAND": function(script, "bool InsertCommand(RenderScriptInstance* i, const Command& command)"),
        "BINDING": function(script, "int RenderScript_SetPolygonOffset(lua_State* L)"),
        "POLYGON_CASE": polygon_case,
    }
    for marker, source in replacements.items():
        harness = harness.replace(marker, source)
    package = os.environ.get("LUA_PKG_CONFIG", "luajit")
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", package], text=True))
    with tempfile.TemporaryDirectory(prefix="defold-polygon-offset-") as temp:
        source = Path(temp) / "test_polygon_offset.cpp"
        binary = Path(temp) / "test_polygon_offset"
        source.write_text(harness)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-O1", "-g",
                        "-fsanitize=address,undefined,float-cast-overflow",
                        "-fno-sanitize-recover=all", source, *flags, "-o", binary], check=True)
        subprocess.run([binary], check=True)


if __name__ == "__main__":
    main()
