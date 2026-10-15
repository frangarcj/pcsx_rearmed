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
local dio=machine.devices[":k573dio"]
local fpga=machine.devices[":k573dio:k573fpga"]
-- Device names contain I/O; luaengine.cpp splits at the first slash, so
-- device.items omits these fields. Match the pinned source save layout instead.
-- Discover the unique source-sorted save-item layout instead of fixed indices.
local metadata={}
for i=0,30000 do
 local v=emu.item(i)
 if v.size==0 then break end
 metadata[#metadata+1]={index=i,size=v.size,count=v.count}
end
local function find_layout(sizes,counts)
 local found={}
 for i=1,#metadata-#sizes+1 do
  local same=true
  for j,size in ipairs(sizes) do
   local v=metadata[i+j-1]
   if v.size~=size or v.count~=(counts[j] or 1) then same=false end
  end
  if same then found[#found+1]=metadata[i].index end
 end
 assert(#found==1,"source save layout is not unique: "..#found)
 return found[1]
end
assert(#metadata<30001,"save registry discovery bound exceeded")
local fpga_base=find_layout({8,4,8,4,8,2,2,2,2,1,1,2,1,1,8,4,4,4,4,2,4,4,4,4,2},{})
local dio_base=find_layout({2,4,1,8,4,2,1,4,4},{[7]=8})
assert(emu.item(dio_base+4):read(0)==19660800,"source digital oscillator")
print("SOURCE_LAYOUT",fpga_base,dio_base)
local fpga_offsets={mp3_frame_counter=21,mp3_start_addr=23,mp3_end_addr=20,mp3_cur_addr=16,fpga_status=11,mpeg_status=24}
local function item(device,name)
 if device==dio then assert(name=="fpga_counter");return emu.item(dio_base+1) end
 return emu.item(fpga_base+assert(fpga_offsets[name],name))
end
local frame_item=item(fpga,"mp3_frame_counter")
local counter_item=item(dio,"fpga_counter")
local start_item=item(fpga,"mp3_start_addr")
local end_item=item(fpga,"mp3_end_addr")
local current_item=item(fpga,"mp3_cur_addr")
local fpga_item=item(fpga,"fpga_status")
local mpeg_item=item(fpga,"mpeg_status")
local trace = assert(io.open(trace_path, "w"))
trace:write("time,frame,boot_rows,pad_rows")
for _, address in ipairs(addresses) do
    trace:write(string.format(",ram_%08x", address))
end
trace:write(",s573_mpeg_frames,s573_latched_samples,s573_start,s573_end,s573_current,s573_pad_bits,s573_fpga,s573_mpeg_state\n")
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
    trace:write(string.format(",%d,%d,%08x,%08x,%08x,%04x,%04x,%04x\n",frame_item:read(0)%65536,counter_item:read(0),start_item:read(0),end_item:read(0),current_item:read(0),machine.ioport.ports[":IN2"]:read(),fpga_item:read(0),mpeg_item:read(0)))
    if tick >= 1042 then
        assert(nextboot == #boot+1 and nextpad == #pads+1, "unconsumed replay")
        machine.video:snapshot()
        print("SONG_FINAL",tick,memory:read_u32(0x80349538))
        machine:exit()
    end
end, "pf4kaa_song_oracle")
