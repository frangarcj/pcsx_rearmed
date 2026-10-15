-- Observe original IIDX2 MMIO; the taps never replace data or write device state.
-- Use a fresh directory/NVRAM tree. Only the explicit operator Start pulse is injected.
local root=assert(os.getenv('PCSX_TWINKLE_RTC_TRACE_DIR'),'set PCSX_TWINKLE_RTC_TRACE_DIR to a fresh writable directory')
root=root:gsub('/+$','')..'/'
local machine=manager.machine
local rtc=machine.devices[':rtc']
local items={}
for name,id in pairs(rtc.items) do
 if name:match('/m_regs$') then items.regs=emu.item(id) end
 if name:match('/m_xram$') then items.xram=emu.item(id) end
 if name:match('/m_cur_reg$') then items.reg=emu.item(id) end
 if name:match('/m_cur_xram_page$') then items.page=emu.item(id) end
 if name:match('/m_SQW_internal_state$') then items.sqw=emu.item(id) end
end
assert(items.regs and items.xram and items.reg and items.page and items.sqw,'RTC source save-item names missing')
local function stamp() return machine.time:as_ticks(1000000000) end
local function snapshot(prefix)
 local f=assert(io.open(root..prefix..'-regs.bin','wb'));f:write(items.regs:read_block(0,64));f:close()
 f=assert(io.open(root..prefix..'-xram.bin','wb'));f:write(items.xram:read_block(0,4096));f:close()
 f=assert(io.open(root..prefix..'-status.csv','w'));f:write(string.format('%d,%d,%d,%d\n',stamp(),items.reg:read(0),items.page:read(0),items.sqw:read(0)));f:close()
end
snapshot('initial')
local log=assert(io.open(root..'rtc-transactions.csv','w'));log:write('ns,operation,xram,offset,value\n')
local count=0
local function access(op,address,data,mask)
 address=address&0x1fffffff
 local xram=address>=0x1f2a0000 and 1 or 0
 local base=xram==1 and 0x1f2a0000 or 0x1f290000
 for lane=0,3 do
  if lane%2==0 and ((mask>>(lane*8))&255)~=0 then
   count=count+1
   log:write(string.format('%d,%s,%d,%d,%02x\n',stamp(),op,xram,(address+lane-base)//2,(data>>(lane*8))&255))
  end
 end
end
local space=machine.devices[':maincpu'].spaces['program']
local taps={}
local selector=0
local input_reads=0
local held_reads=0
for _,alias in ipairs({0,0x80000000,0xa0000000}) do
 for _,region in ipairs({0x1f290000,0x1f2a0000}) do
  taps[#taps+1]=space:install_read_tap(alias+region,alias+region+0x7f,'RTC_read_'..tostring(alias+region),function(a,d,m) access('r',a,d,m) end)
  taps[#taps+1]=space:install_write_tap(alias+region,alias+region+0x7f,'RTC_write_'..tostring(alias+region),function(a,d,m) access('w',a,d,m) end)
 end
 taps[#taps+1]=space:install_write_tap(alias+0x1f220000,alias+0x1f220003,'IO_selector_'..tostring(alias),function(a,d,m)
  if (m&0xff0000)~=0 then selector=(d>>16)&255 end
 end)
 taps[#taps+1]=space:install_read_tap(alias+0x1f220004,alias+0x1f220007,'IO_inputs_'..tostring(alias),function(a,d,m)
  if (m&255)~=0 and selector==7 then
   input_reads=input_reads+1
   if (d&1)==0 then held_reads=held_reads+1 end
  end
 end)
end
_G.twinkle_rtc_trace_taps=taps -- Retain every tap helper throughout the run.
local start=machine.ioport.ports[':IN0'].fields['1 Player Start']
emu.register_frame_done(function()
 local t=machine.time:as_double();start:set_value(t>=65 and t<70 and 1 or 0)
end)
emu.register_stop(function()
 snapshot('final');log:close()
 print(string.format('RTC_SOURCE_TRACE transactions=%d IN0_CPU_reads=%d pressed_CPU_reads=%d',count,input_reads,held_reads))
 for _,tap in ipairs(taps) do tap:remove() end
 _G.twinkle_rtc_trace_taps=nil
end)
