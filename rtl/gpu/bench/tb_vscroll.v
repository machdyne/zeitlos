`timescale 1ns/1ps
module tb_vscroll;
    reg clk=0,rst=1; always #5 clk=~clk;
    reg wb_cyc=0,wb_stb=0,wb_we=0; reg [31:0] wb_adr=0,wb_dat=0;
    wire [31:0] wb_dat_o; wire wb_ack;
    wire m_cyc,m_stb,m_we; wire [3:0] m_sel; wire [31:0] m_adr,m_dat_o;
    reg [31:0] m_dat_i; reg m_ack=0;
    wire s_cyc,s_stb,s_we; wire [3:0] s_sel; wire [31:0] s_adr;
    reg [31:0] s_dat_i=0; reg s_ack=0;
    wire [11:0] glyph_addr; wire busy;
    gpu_blit_wb dut(.clk(clk),.rst(rst),
      .wb_cyc_i(wb_cyc),.wb_stb_i(wb_stb),.wb_we_i(wb_we),.wb_sel_i(4'hF),
      .wb_adr_i(wb_adr),.wb_dat_i(wb_dat),.wb_ack_o(wb_ack),.wb_dat_o(wb_dat_o),
      .m_cyc_o(m_cyc),.m_stb_o(m_stb),.m_we_o(m_we),.m_sel_o(m_sel),
      .m_adr_o(m_adr),.m_dat_o(m_dat_o),.m_dat_i(m_dat_i),.m_ack_i(m_ack),
      .s_cyc_o(s_cyc),.s_stb_o(s_stb),.s_we_o(s_we),.s_sel_o(s_sel),
      .s_adr_o(s_adr),.s_dat_i(s_dat_i),.s_ack_i(s_ack),
      .glyph_addr_o(glyph_addr),.glyph_data_i(8'h00),.busy(busy));
    localparam VRAM_BASE=32'h20000000, WPL=20;
    reg [31:0] vram[0:9599];
    integer i,errors;
    always @(posedge clk) begin
      m_ack<=0;
      if(!rst && m_cyc && m_stb) begin
        m_dat_i<=vram[(m_adr-VRAM_BASE)>>2];
        if(m_we) vram[(m_adr-VRAM_BASE)>>2]<=m_dat_o;
        m_ack<=1; end end
    task wb_write(input [31:0] a,input [31:0] d);
      begin @(posedge clk); wb_adr<=a; wb_dat<=d; wb_we<=1; wb_cyc<=1; wb_stb<=1;
        @(posedge clk); while(!wb_ack) @(posedge clk);
        wb_cyc<=0; wb_stb<=0; wb_we<=0; @(posedge clk); end endtask
    task wait_done; integer g; begin g=0;
      while(!busy && g<50) begin @(posedge clk); g=g+1; end
      while(busy) @(posedge clk); @(posedge clk); end endtask
    localparam R_CTRL=0,R_DX=2,R_DY=3,R_W=4,R_H=5,R_SA=12,R_SS=13,R_SH=14;
    localparam C_START=32'h1, C_CLIP=32'h4;
    /* exactly what z_fb_hw_blit_vram() does */
    task blit_vram(input integer sx,input integer sy,
                   input integer dx,input integer dy,
                   input integer w,input integer h);
      integer sbit0, sword, sshift; reg [31:0] prime;
      begin
        sbit0 = sx - (dx % 32);
        if (sbit0 >= 0) begin sword = sbit0/32; sshift = sbit0%32; prime=0; end
        else begin sword = 0; sshift = sbit0+32; prime=32'h100; end
        wb_write(R_DX,dx); wb_write(R_DY,dy); wb_write(R_W,w); wb_write(R_H,h);
        wb_write(R_SA, VRAM_BASE + sy*80 + sword*4);
        wb_write(R_SS, 80);
        wb_write(R_SH, prime | sshift);
        wb_write(R_CTRL, C_START | C_CLIP);
        wait_done();
      end endtask
    task check(input integer x0,input integer w,input integer sy,input integer dy,
               input [255:0] label);
      integer px,bad; reg exp,got;
      begin
        bad=0;
        for(px=x0; px<x0+w; px=px+1) begin
          exp = vram[sy*WPL + (px>>5)][px%32];
          got = vram[dy*WPL + (px>>5)][px%32];
          if(exp!==got) bad=bad+1;
        end
        if(bad) begin
          $display("FAIL: %0s: %0d of %0d pixels wrong", label, bad, w);
          errors=errors+1;
        end
      end endtask
    integer x0,w,row;
    initial begin
      errors=0;
      for(i=0;i<9600;i=i+1) vram[i]=0;
      repeat(8) @(posedge clk); rst=0; repeat(4) @(posedge clk);

      /* a distinctive pattern in the source rows */
      for(row=100;row<110;row=row+1)
        for(i=0;i<WPL;i=i+1) vram[row*WPL+i] = 32'hA5A50000 ^ (row<<8) ^ i;

      $display("=== VRAM->VRAM copy, same x, varying alignment ===");
      /* word-aligned */
      x0=32; w=320;
      blit_vram(x0,100, x0,200, w,1);
      check(x0,w,100,200,"aligned x=32");
      /* the case the apps hit: window content starts at x=44 */
      x0=44; w=320;
      blit_vram(x0,101, x0,201, w,1);
      check(x0,w,101,201,"unaligned x=44");
      /* a couple more offsets */
      x0=12; w=200;
      blit_vram(x0,102, x0,202, w,1);
      check(x0,w,102,202,"unaligned x=12");
      x0=63; w=100;
      blit_vram(x0,103, x0,203, w,1);
      check(x0,w,103,203,"unaligned x=63");
      /* multi-row, like a real scroll */
      blit_vram(44,100, 44,300, 320,8);
      for(row=0;row<8;row=row+1) check(44,320,100+row,300+row,"multi-row x=44");

      if(errors==0) $display("RESULT: PASS -- hardware copy is correct");
      else $display("RESULT: FAIL (%0d)", errors);
      $finish;
    end
endmodule
