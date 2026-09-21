// zfpga synth: the Verilog subset, all at once. Checked against yosys.
module top(input clk, input rst_n, input en, input [3:0] a, input [3:0] b,
           input [1:0] sel, input ci,
           output reg [3:0] q, output [4:0] sum, output lt, output ge, output eqz,
           output [3:0] mx, output [3:0] sh, output par, output bit_sel,
           output reg [2:0] st, output [3:0] cnt_o);
    localparam IDLE = 3'd0, RUN = 3'd1, DONE = 3'd4;
    reg [3:0] cnt = 4'd9;
    reg [3:0] m;

    assign sum = a + b + ci;                // context width: the carry lands in sum[4]
    assign lt = a < b;
    assign ge = a >= b;
    assign eqz = (a == 4'd0) && !en;
    assign sh = (a << 1) ^ (b >> sel);      // constant and variable shifts
    assign par = ^{a, b};
    assign bit_sel = a[sel];                // a variable index
    assign cnt_o = cnt;
    assign mx = en ? m : {2{sel}};

    always @(*) begin
        case (sel)
            2'd0: m = a & b;
            2'd1: m = a | b;
            2'd2: m = {b[1:0], a[3:2]};
            default: m = ~a;
        endcase
    end

    always @(posedge clk or negedge rst_n)
        if (!rst_n) q <= 4'b1010;
        else if (en) q <= q - b;

    always @(posedge clk) begin
        if (a == 4'hf) cnt <= 0;
        else cnt <= cnt + 1;
        case (st)
            IDLE: if (en) st <= RUN;
            RUN: if (cnt == 4'd3) st <= DONE;
            DONE: st <= IDLE;
            default: st <= IDLE;
        endcase
    end
endmodule
