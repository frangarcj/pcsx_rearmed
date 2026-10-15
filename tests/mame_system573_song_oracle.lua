-- Original PF4 KAA: replay boot/menu events, then align pads to the song tick.
-- Use independently installed MAME NVRAM. These paths are explicit runner inputs.
local machine = manager.machine
assert(machine.system.name == "pcnfrk4mk", "requires original PF4 KAA")
local input_path = assert(os.getenv("PCSX_PF4_INPUTS"), "missing PCSX_PF4_INPUTS")
local trace_path = assert(os.getenv("PCSX_PF4_TRACE"), "missing PCSX_PF4_TRACE")
local function field(tag, mask)
    for _, value in pairs(machine.ioport.ports[tag].fields) do
        if value.mask == mask then return value end
    end
    error("missing source input " .. tag .. "/" .. mask)
end
local controls = {
    boot_cd=field(":IN1",8), test=field(":IN3",0x400),
    coin1=field(":IN1",0x01000000), p1start=field(":IN2",0x8000)
}
for i, mask in ipairs({0x100,0x200,0x400,0x800,0x1000,0x4000}) do
    controls["p1button" .. i] = field(":IN2", mask)
end
local boot, pads = {}, {}
for line in io.lines(input_path) do
    local frame, name, value = line:match("^(%d+),([^,]+),(%d+)$")
    if frame then
        frame, value = tonumber(frame), tonumber(value)
        assert(controls[name] and (value == 0 or value == 1), line)
        if name:match("^p1button") then
            -- Native frame 12758 observes tick 1; inputs at the next VBlank
            -- precede that frame's game update. Preserve this boundary phase.
            pads[#pads+1] = {frame-12758, name, value}
        else
            boot[#boot+1] = {frame, name, value}
        end
    end
end
assert(#boot == 19 and #pads == 1476, "requires the recorded 1495-event replay")
local memory = machine.devices[":maincpu"].spaces["program"]
local addresses = {0x803493f8,0x803493fc,0x80349408,0x80349414,0x80349424,
                   0x80349428,0x8034942c,0x80349514,0x80349520,0x80349538}
local trace = assert(io.open(trace_path, "w"))
trace:write("time,frame,boot_rows,pad_rows")
for _, address in ipairs(addresses) do
    trace:write(string.format(",ram_%08x", address))
end
trace:write("\n")
emu.register_stop(function() trace:close() end)
local nextboot, nextpad = 1, 1
emu.register_frame_done(function()
    local time = machine.time:as_double()
    -- The old cold-boot conversion is used only for boot/menu navigation.
    local frame = math.floor(time*14400/(8152128004/33868800))
    while nextboot <= #boot and boot[nextboot][1] <= frame do
        local event = boot[nextboot]
        controls[event[2]]:set_value(event[3])
        nextboot = nextboot+1
    end
    if time < 180 then return end
    local tick = memory:read_u32(0x803493f8)
    while tick > 0 and nextpad <= #pads and pads[nextpad][1] <= tick do
        local event = pads[nextpad]
        controls[event[2]]:set_value(event[3])
        print("SONG_INPUT", tick, event[1], event[2], event[3])
        nextpad = nextpad+1
    end
    trace:write(string.format("%.9f,%d,%d,%d",time,frame,nextboot-1,nextpad-1))
    for _, address in ipairs(addresses) do
        trace:write(string.format(",%08x",memory:read_u32(address)))
    end
    trace:write("\n")
    if tick >= 1042 then
        assert(nextboot == #boot+1 and nextpad == #pads+1, "unconsumed replay")
        machine.video:snapshot()
        print("SONG_FINAL",tick,memory:read_u32(0x80349538))
        machine:exit()
    end
end, "pf4kaa_song_oracle")
