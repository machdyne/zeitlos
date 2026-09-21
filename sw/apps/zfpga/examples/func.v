// zfpga synth: functions -- a CRC step with a loop, called twice.
module top(input clk, input rst, input [7:0] d, input valid, output reg [15:0] crc,
           output [15:0] peek, output [3:0] m);
    function [15:0] crc16_byte;
        input [15:0] c;
        input [7:0] data;
        integer i;
        reg [15:0] x;
        begin
            x = c;
            for (i = 0; i < 8; i = i + 1)
                x = (x[15] ^ data[7 - i]) ? ((x << 1) ^ 16'h1021) : (x << 1);
            crc16_byte = x;
        end
    endfunction
    function [3:0] max4(input [3:0] p, input [3:0] q);
        max4 = p > q ? p : q;
    endfunction
    always @(posedge clk)
        if (rst) crc <= 16'hFFFF;
        else if (valid) crc <= crc16_byte(crc, d);
    assign peek = crc16_byte(16'h0000, d);
    assign m = max4(d[3:0], d[7:4]);
endmodule
