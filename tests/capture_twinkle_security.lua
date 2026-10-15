-- Read-only observation of the original IIDX2 security port and EEPROM bytes.
local root=assert(os.getenv('PCSX_TWINKLE_SECURITY_TRACE_DIR')):gsub('/+$','')..'/'
local m=manager.machine
local item
for name,id in pairs(m.devices[':security'].items) do if name:match('/m_data$') then item=emu.item(id) end end
assert(item,'source security m_data missing')
local function image(name) local f=assert(io.open(root..name..'.bin','wb'));f:write(item:read_block(0,256));f:close() end
image('initial')
local log=assert(io.open(root..'transactions.csv','w'));log:write('operation,data,mask\n')
local taps={};local count=0
for _,alias in ipairs({0,0x80000000,0xa0000000}) do
 taps[#taps+1]=m.devices[':maincpu'].spaces['program']:install_write_tap(alias+0x1f270000,alias+0x1f270003,'security_w'..alias,function(a,d,mask)
  log:write(string.format('w,%08x,%08x\n',d,mask));count=count+1
 end)
 taps[#taps+1]=m.devices[':maincpu'].spaces['program']:install_read_tap(alias+0x1f280000,alias+0x1f280003,'security_r'..alias,function(a,d,mask)
  log:write(string.format('r,%08x,%08x\n',d,mask));count=count+1
 end)
end
_G.twinkle_security_taps=taps
emu.register_stop(function()
 image('final');log:close();for _,tap in ipairs(taps) do tap:remove() end
 _G.twinkle_security_taps=nil;print('TWINKLE_SOURCE_SECURITY rows='..count)
end)
