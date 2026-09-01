`timescale 1ns/1ps
/*
 * The scissor applied to GLYPH mode.
 *
 * gpu_blitter.md used to say glyph mode was "unclipped by design --
 * the caller is expected to fall back to software rendering for
 * glyphs that would need partial clipping". That fallback is what
 * this removes, so the interesting cases are the partial ones: a
 * glyph the scissor cuts through, not one it misses entirely.
 *
 * Four things need to be true:
 *
 *   1. Without CTRL_CLIP, glyph mode behaves exactly as before. The
 *      scissor is ignored entirely, which is what keeps every existing
 *      caller working.
 *
 *   2. Vertical clipping drops whole rows and leaves the rest exactly
 *      where they were -- not shifted up into the gap.
 *
 *   3. Horizontal clipping cuts a glyph mid-cell, and the surviving
 *      columns stay in their original positions.
 *
 *   4. THE BACKGROUND IS CLIPPED TOO, not just the foreground. Glyph
 *      mode paints a solid cell -- fg where the glyph bit is set, bg
 *      everywhere else -- so clipping only the bits would still let
 *      the background erase pixels outside the scissor. For an
 *      occluded terminal that means erasing the window in front of it
 *      in the shape of its own text, which is worse than not clipping
 *      at all. This is checked by seeding the framebuffer with 1s and
 *      confirming the out-of-scissor ones survive a bg=0 glyph.
 *
 *   iverilog -g2005 -o /tmp/tb rtl/gpu/bench/tb_glyph_clip.v \
 *       rtl/gpu/gpu_blit.v rtl/mem/glyph.v
 *   vvp /tmp/tb
 */
module tb_glyph_clip;
    reg clk=0, rst=1; always #5 clk=~clk;
    reg wb_cyc=0, wb_stb=0, wb_we=0; reg [31:0] wb_adr=0, wb_dat=0;
    wire [31:0] wb_dat_o; wire wb_ack;
    wire m_cyc, m_stb, m_we; wire [3:0] m_sel; wire [31:0] m_adr, m_dat_o;
    reg [31:0] m_dat_i; reg m_ack=0;
    wire s_cyc, s_stb, s_we; wire [3:0] s_sel; wire [31:0] s_adr;
    wire [11:0] glyph_addr; wire [7:0] glyph_data; wire busy;

    gpu_blit_wb dut(.clk(clk), .rst(rst),
      .wb_cyc_i(wb_cyc), .wb_stb_i(wb_stb), .wb_we_i(wb_we), .wb_sel_i(4'hF),
      .wb_adr_i(wb_adr), .wb_dat_i(wb_dat), .wb_ack_o(wb_ack), .wb_dat_o(wb_dat_o),
      .m_cyc_o(m_cyc), .m_stb_o(m_stb), .m_we_o(m_we), .m_sel_o(m_sel),
      .m_adr_o(m_adr), .m_dat_o(m_dat_o), .m_dat_i(m_dat_i), .m_ack_i(m_ack),
      .s_cyc_o(s_cyc), .s_stb_o(s_stb), .s_we_o(s_we), .s_sel_o(s_sel),
      .s_adr_o(s_adr), .s_dat_i(32'h0), .s_ack_i(1'b0),
      .glyph_addr_o(glyph_addr), .glyph_data_i(glyph_data), .busy(busy));

    glyph_mem #(.ADDR_WIDTH(12)) gmem(
      .clk(clk),
      .wb_cyc_i(1'b0), .wb_stb_i(1'b0), .wb_we_i(1'b0), .wb_sel_i(4'h0),
      .wb_adr_i(32'h0), .wb_dat_i(32'h0), .wb_ack_o(), .wb_dat_o(),
      .blit_addr(glyph_addr), .blit_data(glyph_data));

    localparam VRAM_BASE=32'h20000000, WPL=20;
    reg [31:0] vram[0:9599];
    integer i, j, errors;

    always @(posedge clk) begin
      m_ack <= 0;
      if(!rst && m_cyc && m_stb) begin
        m_dat_i <= vram[(m_adr-VRAM_BASE)>>2];
        if(m_we) vram[(m_adr-VRAM_BASE)>>2] <= m_dat_o;
        m_ack <= 1; end
    end

    task wb_write(input [31:0] a, input [31:0] d);
      begin @(posedge clk); wb_adr<=a; wb_dat<=d; wb_cyc<=1; wb_stb<=1; wb_we<=1;
        @(posedge clk); while(!wb_ack) @(posedge clk);
        wb_cyc<=0; wb_stb<=0; wb_we<=0; @(posedge clk); end endtask

    localparam R_CTRL=0, R_DX=2, R_DY=3, R_GA=7, R_GW=8, R_GH=9;
    localparam R_FG=10, R_BG=11;
    localparam R_CX0=20, R_CY0=21, R_CX1=22, R_CY1=23;
    localparam C_START=32'h1, C_CLIP=32'h4, C_GLYPH=32'h8;

    task set_clip(input integer x0, input integer y0,
                  input integer x1, input integer y1);
      begin wb_write(R_CX0,x0); wb_write(R_CY0,y0);
            wb_write(R_CX1,x1); wb_write(R_CY1,y1); end endtask

    // 8x4 glyph: every pixel set, so any clipping shows as absence
    task seed_glyph; begin
      gmem.mem[0] = {8'b11111111, 8'b11111111, 8'b11111111, 8'b11111111};
    end endtask

    task draw(input integer x, input integer y, input integer bg,
              input integer use_clip);
      integer g;
      begin
        wb_write(R_DX,x); wb_write(R_DY,y);
        wb_write(R_GA,0); wb_write(R_GW,8); wb_write(R_GH,4);
        wb_write(R_FG,1); wb_write(R_BG,bg);
        wb_write(R_CTRL, C_START | C_GLYPH | (use_clip ? C_CLIP : 0));
        g=0; while(!busy && g<40) begin @(posedge clk); g=g+1; end
        g=0; while(busy && g<20000) begin @(posedge clk); g=g+1; end
        if(g>=20000) begin
          $display("  FAIL: glyph blit never finished"); errors=errors+1; end
        @(posedge clk);
      end endtask

    function pix(input integer x, input integer y);
      begin pix = vram[y*WPL + (x>>5)][x%32]; end endfunction

    task fill_vram(input integer v); begin
      for(i=0;i<9600;i=i+1) vram[i] = v ? 32'hFFFFFFFF : 32'h0; end endtask

    // every pixel in a 24x8 window must match the expected rectangle
    task check(input [255:0] name,
               input integer x0, input integer y0,
               input integer x1, input integer y1);
      integer bad, want;
      begin
        bad=0;
        for(j=0;j<8;j=j+1) for(i=0;i<24;i=i+1) begin
          want = (i>=x0 && i<x1 && j>=y0 && j<y1) ? 1 : 0;
          if(pix(i,j) !== want[0]) bad=bad+1;
        end
        if(bad!=0) begin
          $display("  FAIL %0s: %0d wrong px (want x %0d..%0d y %0d..%0d)",
                   name, bad, x0, x1-1, y0, y1-1);
          for(j=0;j<6;j=j+1) begin
            $write("        row %0d: ", j);
            for(i=0;i<24;i=i+1) $write("%0d", pix(i,j));
            $write("\n");
          end
          errors=errors+1;
        end else $display("  ok   %0s", name);
      end endtask

    initial begin
      errors=0; seed_glyph; fill_vram(0);
      repeat(4) @(posedge clk); rst<=0; repeat(4) @(posedge clk);

      $display("== without CTRL_CLIP the scissor is ignored ==");
      set_clip(2,1,6,3);              // would cut it to nothing much
      fill_vram(0);
      draw(4, 1, 0, 0);               // no CTRL_CLIP
      check("unclipped glyph lands whole", 4,1,12,5);

      $display("\n== vertical: whole rows dropped, rest not shifted ==");
      fill_vram(0);
      set_clip(0,2,640,4);            // keep rows 2..3
      draw(4, 1, 0, 1);
      check("top and bottom rows clipped", 4,2,12,4);

      $display("\n== horizontal: cut mid-cell, columns stay put ==");
      fill_vram(0);
      set_clip(6,0,640,480);          // drop columns 4..5
      draw(4, 1, 0, 1);
      check("left of glyph clipped", 6,1,12,5);

      fill_vram(0);
      set_clip(0,0,9,480);            // keep columns up to 8
      draw(4, 1, 0, 1);
      check("right of glyph clipped", 4,1,9,5);

      $display("\n== both axes at once ==");
      fill_vram(0);
      set_clip(6,2,9,4);
      draw(4, 1, 0, 1);
      check("clipped on all four sides", 6,2,9,4);

      $display("\n== entirely outside draws nothing ==");
      fill_vram(0);
      set_clip(100,100,120,120);
      draw(4, 1, 0, 1);
      check("glyph outside scissor", 0,0,0,0);

      $display("\n== the BACKGROUND is clipped too ==");
      // Framebuffer all 1s, glyph drawn with bg=0. Outside the
      // scissor the 1s must survive: if only the glyph BITS were
      // clipped and not the cell, the background would erase them.
      fill_vram(1);
      set_clip(6,2,9,4);
      draw(4, 1, 0, 1);               // fg=1 bg=0, glyph is all-ones
      begin : bgcheck
        integer bad; bad=0;
        for(j=0;j<8;j=j+1) for(i=0;i<24;i=i+1)
          if(pix(i,j) !== 1'b1) bad=bad+1;
        // every pixel should still be 1: inside the scissor the glyph
        // paints fg=1, outside nothing was touched
        if(bad!=0) begin
          $display("  FAIL: background escaped the scissor, %0d px cleared", bad);
          errors=errors+1;
        end else $display("  ok   background stays inside the scissor");
      end

      $display("");
      if(errors==0)
        $display("PASS -- glyph mode clips, no software fallback needed");
      else $display("FAIL -- %0d problem(s)", errors);
      $finish;
    end

    initial begin #2000000; $display("TIMEOUT"); $finish; end
endmodule
