`timescale 1ns/1ps
/*
 * Does an unaligned VRAM-to-VRAM copy write its FIRST (partial)
 * destination word?
 *
 * tb_vscroll.v compares destination against source and passes -- but
 * it only checks pixels inside the copy region, and it seeds every
 * source row from the same generator. If the first word were skipped,
 * the destination would keep its OLD content -- and if that old
 * content happened to match, the check would pass.
 *
 * So this seeds the destination with a value that CANNOT be confused
 * with the source, and checks three things separately:
 *   - pixels left of dst_x must be untouched (the mask must protect them)
 *   - pixels from dst_x to the end of the first word must be copied
 *   - pixels in later whole words must be copied
 */
module tb_edge;
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
      while(!busy && g<60) begin @(posedge clk); g=g+1; end
      while(busy) @(posedge clk); @(posedge clk); end endtask
    localparam R_CTRL=0,R_DX=2,R_DY=3,R_W=4,R_H=5,R_SA=12,R_SS=13,R_SH=14;
    localparam C_START=32'h1, C_CLIP=32'h4;
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

    integer x0,w,px,sy,dy,bad_left,bad_first,bad_rest,firstend;
    reg exp,got;
    initial begin
      errors=0;
      for(i=0;i<9600;i=i+1) vram[i]=0;
      repeat(8) @(posedge clk); rst=0; repeat(4) @(posedge clk);

      $display("=== does the first PARTIAL destination word get written? ===");

      for (x0 = 44; x0 <= 46; x0 = x0 + 2) begin
        sy = 62; dy = 53; w = 256;

        /* source: all ones. destination: all zeros. Any pixel left as
           zero inside the region was not copied; any pixel set outside
           it was wrongly written. No ambiguity either way. */
        for(i=0;i<WPL;i=i+1) begin
          vram[sy*WPL+i] = 32'hFFFFFFFF;
          vram[dy*WPL+i] = 32'h00000000;
        end

        blit_vram(x0,sy, x0,dy, w,1);

        bad_left=0; bad_first=0; bad_rest=0;
        firstend = ((x0/32)+1)*32;   /* end of the first destination word */

        for(px=0; px<640; px=px+1) begin
          got = vram[dy*WPL + (px/32)][px%32];
          if (px < x0) begin
            if (got !== 1'b0) bad_left = bad_left + 1;
          end else if (px < x0+w) begin
            if (got !== 1'b1) begin
              if (px < firstend) bad_first = bad_first + 1;
              else bad_rest = bad_rest + 1;
            end
          end else begin
            if (got !== 1'b0) bad_left = bad_left + 1;
          end
        end

        $display("  dst_x=%0d: left-of-region wrong=%0d, FIRST word missing=%0d of %0d, later words missing=%0d",
          x0, bad_left, bad_first, firstend-x0, bad_rest);
        if (bad_left || bad_first || bad_rest) errors = errors + 1;
      end

      if(errors==0) $display("RESULT: PASS -- first partial word is written correctly");
      else $display("RESULT: FAIL (%0d)", errors);
      $finish;
    end
endmodule
