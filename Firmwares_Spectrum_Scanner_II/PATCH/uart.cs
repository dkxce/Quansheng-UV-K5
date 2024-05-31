byte MaxLen = 200;
byte[] ba = Encoding.ASCII.GetBytes("fsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaffsfasfsafsafsafsafsafsafsaf");
byte[] pckt = new byte[256];

System.IO.Ports.SerialPort sp = new System.IO.Ports.SerialPort("COM5", 38400, System.IO.Ports.Parity.None, 8, System.IO.Ports.StopBits.One);
// FF0009
pckt[0] = 0xFF; // BEGIN OF PACKET            
pckt[1] = 0x00; // COMMAND
pckt[2] = 0x09; // COMMAND
pckt[3] = (byte)(ba.Length > MaxLen ? MaxLen : ba.Length); // DATA LENGTH
Array.Copy(ba, 0, pckt, 4, ba.Length > MaxLen ? MaxLen : ba.Length);
lock (sp)
{
    try
    {
        sp.Open();
        sp.Write(pckt, 0, pckt.Length);
    }
    catch (Exception ex) 
    {
    };
}
sp.Close();
return;