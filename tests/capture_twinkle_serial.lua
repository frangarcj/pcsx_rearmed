-- Original IIDX2 Super-I/O bus observation; no replaced data or device state.
local root=assert(os.getenv('PCSX_TWINKLE_SERIAL_TRACE_DIR')):gsub('/+$','')..'/'
local m=manager.machine
local log=assert(io.open(root..'transactions.csv','w'));log:write('ns,operation,offset,value\n')
local count=0
local function access(op,a,d,mask)
 a=a&0x1fffffff
 for lane=0,3 do
  if ((a+lane)&1)==0 and ((mask>>(lane*8))&255)~=0 then
   count=count+1;log:write(string.format('%d,%s,%03x,%02x\n',m.time:as_ticks(1000000000),op,(a+lane-0x1f210000)//2,(d>>(lane*8))&255))
  end
 end
end
local taps={}
for _,alias in ipairs({0,0x80000000,0xa0000000}) do
 local s=m.devices[':maincpu'].spaces['program']
 taps[#taps+1]=s:install_read_tap(alias+0x1f210000,alias+0x1f2107ff,'uart_read'..alias,function(a,d,mask)access('r',a,d,mask)end)
 taps[#taps+1]=s:install_write_tap(alias+0x1f210000,alias+0x1f2107ff,'uart_write'..alias,function(a,d,mask)access('w',a,d,mask)end)
end
_G.twinkle_serial_taps=taps
local start=m.ioport.ports[':IN0'].fields['1 Player Start']
emu.register_frame_done(function() local t=m.time:as_double();start:set_value(t>=65 and t<70 and 1 or 0) end)
emu.register_stop(function()
 log:close();local f=assert(io.open(root..'final.csv','w'));f:write(string.format('ns,%d\n',m.time:as_ticks(1000000000)))
 for i=1,2 do
  local u=m.devices[':fdc37c665gt:uart'..i]
  for name,id in pairs(u.items) do
   for _,reg in ipairs({'lcr','lsr','fcr','dl'}) do
    if name:match('/m_regs%.'..reg..'$') then f:write(string.format('%d,%s,%d\n',i,reg,emu.item(id):read(0))) end
   end
  end
 end
 f:close();for _,tap in ipairs(taps) do tap:remove() end
 _G.twinkle_serial_taps=nil;print('TWINKLE_SOURCE_SERIAL transactions='..count)
end)
