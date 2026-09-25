/*
 * tb_mpu.v -- unit testbench for rtl/mpu.v (wb_mpu)
 *
 * A "CPU" issues fetches, loads and stores; a reference model written
 * independently of the RTL decides what each one should do, and every
 * outcome is checked: whether it reached the downstream bus at all
 * (a blocked request must never assert downstream cyc/stb), the data
 * returned, and the fault registers.
 *
 *   iverilog -g2005 -o tb_mpu rtl/tb/tb_mpu.v rtl/mpu.v && ./tb_mpu
 *   iverilog -g2005 -Ptb_mpu.XLATE=0 ...     (zeitlos32: no 0x8 on bus)
 */

`timescale 1ns / 1ps
//
// 512MB main memory (`MAIN_512MB): -Ptb_mpu.MAIN_512=1. Adding
// -Ptb_mpu.DUT_512=0 runs that model against the old rules, and must
// FAIL -- stores above 0x5000_0000 let through.

module tb_mpu;

    parameter XLATE = 1;
    // Main memory 0x4000_0000-0x5fff_ffff (`MAIN_512MB). The reference
    // model below follows MAIN_512 and the address generator adds a
    // 0x5 region only then, so the default run is unchanged. DUT_512
    // sets the design under test separately: running MAIN_512=1 against
    // DUT_512=0 must FAIL -- that is the hole the change closes, stores
    // above 0x5000_0000 outside the app's own block being let through.
    parameter MAIN_512 = 0;
    parameter DUT_512 = MAIN_512;
    parameter SEED = 1;
    parameter NOPS = 20000;

    localparam CFG   = 32'h9000_0100;
    localparam KTEXT = 32'h4001_0000;
    localparam GATE  = 32'h4000_0100;
    localparam BASE  = 32'h4008_0000;
    localparam SIZE  = 32'h0000_4000;
    localparam MASK  = 16'hF7FF;          // SD card nibble (0xB) off

    reg clk, rst;
    reg [31:0] adr, dat;
    reg we, stb, cyc, instr;
    reg [3:0] sel;
    wire [31:0] c_dat;
    wire c_ack;
    wire d_stb, d_cyc;
    reg [31:0] d_dat;
    reg d_ack;
    reg [31:0] mtu_base;
    wire irq;

    wb_mpu #(.MAIN_512(DUT_512), .XLATE_ON_BUS(XLATE)) dut (
        .wb_clk_i(clk), .wb_rst_i(rst),
        .c_adr_i(adr), .c_dat_i(dat), .c_we_i(we), .c_sel_i(sel),
        .c_stb_i(stb), .c_cyc_i(cyc), .c_instr_i(instr),
        .c_dat_o(c_dat), .c_ack_o(c_ack),
        .d_stb_o(d_stb), .d_cyc_o(d_cyc), .d_dat_i(d_dat), .d_ack_i(d_ack),
        .mtu_base_i(mtu_base), .irq_o(irq)
    );

    initial clk = 0;
    always #5 clk = ~clk;

    // -- downstream slave: random latency, returns a function of the
    //    address so pass-through data is checkable
    integer seed, errors, lat, passed_down;
    always @(posedge clk) begin
        d_ack <= 1'b0;
        if (d_cyc && d_stb && !d_ack) begin
            if (lat == 0) begin
                d_ack <= 1'b1;
                d_dat <= adr ^ 32'hA5A5_5A5A;
                passed_down = passed_down + 1;
                lat = $unsigned($random(seed)) % 4;
            end else lat = lat - 1;
        end
    end

    // -- reference model ----------------------------------------------
    reg m_en, m_enf, m_priv;
    reg [31:0] m_count;
    reg m_fvalid;
    reg [31:0] m_faddr;
    reg [3:0] m_freason;

    function in_own;                         // model: inside the block
        input [31:0] a;
    begin
        if (XLATE && a[31:28] == 4'h8 && mtu_base != 0)
            in_own = (a[27:0] < SIZE);
        else
            in_own = (a >= mtu_base) && (a < mtu_base + SIZE) &&
                     (a[31:28] == 4'h4);
    end
    endfunction

    function [3:0] phys_nib;
        input [31:0] a;
    begin
        phys_nib = (XLATE && a[31:28] == 4'h8 && mtu_base != 0) ? 4'h4 : a[31:28];
    end
    endfunction

    function is_kernel_code;
        input [31:0] a;
    begin
        if (XLATE && a[31:28] == 4'h8 && mtu_base != 0) is_kernel_code = 0;
        else is_kernel_code = (a < 32'h2000) || (a >= 32'h4000_0000 && a < KTEXT);
    end
    endfunction

    // expected reason (0 = allowed)
    function [3:0] expect_why;
        input [31:0] a;
        input w;
        input f;
        reg [3:0] n;
    begin
        expect_why = 0;
        n = phys_nib(a);
        if (m_en && !m_priv) begin
            if (f) begin
                if (is_kernel_code(a)) begin
                    if (a != GATE && a != 32'h10) expect_why = 4;
                end else if (!in_own(a)) expect_why = 1;
            end else if (!MASK[n]) expect_why = 3;
            else if (w) begin
                if (n == 4'h4 || (MAIN_512 && n == 4'h5)) begin
                    if (!in_own(a)) expect_why = 1;
                end else if (n == 4'h0 || n == 4'h1 || n == 4'h9 ||
                         a[31:8] == 24'h7000_01 || a[31:2] == (32'h7000_0218 >> 2))
                    expect_why = 2;
            end
        end
    end
    endfunction

    // -- driver ---------------------------------------------------------
    reg [31:0] got;
    integer before, t;
    reg [3:0] w_;
    reg blocked_expected;

    task bus;
        input [31:0] a;
        input w;
        input f;
        input [31:0] d;
    begin
        @(posedge clk);
        adr <= a; we <= w; instr <= f; dat <= d; sel <= w ? 4'hF : 4'h0;
        stb <= 1; cyc <= 1;
        before = passed_down;
        t = 0;
        @(posedge clk);
        while (!c_ack) begin
            t = t + 1;
            if (t > 100) begin $display("TIMEOUT %h", a); $finish; end
            @(posedge clk);
        end
        got = c_dat;
        stb <= 0; cyc <= 0; we <= 0; instr <= 0;
    end
    endtask

    // access that is not to the MPU's own registers, checked vs model
    task access;
        input [31:0] a;
        input w;
        input f;
    begin
        w_ = expect_why(a, w, f);
        blocked_expected = (w_ != 0) && m_enf;
        bus(a, w, f, 32'h1234_5678);
        if (blocked_expected) begin
            if (passed_down != before) begin
                $display("ERROR: blocked access reached the bus: %h w=%0d f=%0d why=%0d", a, w, f, w_);
                errors = errors + 1;
            end
            if (got !== 32'h0) begin
                $display("ERROR: blocked access returned %h", got); errors = errors + 1;
            end
        end else begin
            if (passed_down != before + 1) begin
                $display("ERROR: allowed access did not reach the bus: %h w=%0d f=%0d why=%0d priv=%0d", a, w, f, w_, m_priv);
                errors = errors + 1;
            end
            if (!w && got !== (a ^ 32'hA5A5_5A5A)) begin
                $display("ERROR: data %h for %h", got, a); errors = errors + 1;
            end
        end
        if (w_ != 0) begin
            m_count = m_count + 1;
            if (!m_fvalid) begin m_fvalid = 1; m_faddr = a; m_freason = w_; end
        end
        // model privilege: a completed fetch decides it, unless blocked
        if (f && !blocked_expected) m_priv = !m_en || is_kernel_code(a);
    end
    endtask

    task cfgw;
        input [3:0] r;
        input [31:0] d;
    begin
        bus(CFG + r*4, 1, 0, d);
    end
    endtask

    task cfgr;
        input [3:0] r;
    begin
        bus(CFG + r*4, 0, 0, 0);
    end
    endtask

    task check_fault;
    begin
        cfgr(8);
        if (got !== m_count) begin
            $display("ERROR: COUNT %0d expected %0d", got, m_count); errors = errors + 1;
        end
        cfgr(7);
        if (got[31] !== m_fvalid || (m_fvalid && got[27:24] !== m_freason)) begin
            $display("ERROR: FAULT_INFO %h expected valid=%0d reason=%0d", got, m_fvalid, m_freason);
            errors = errors + 1;
        end
        if (m_fvalid) begin
            cfgr(5);
            if (got !== m_faddr) begin
                $display("ERROR: FAULT_ADDR %h expected %h", got, m_faddr); errors = errors + 1;
            end
        end
        if (irq !== m_fvalid) begin
            $display("ERROR: irq=%0d expected %0d", irq, m_fvalid); errors = errors + 1;
        end
    end
    endtask

    task clear_fault;
    begin
        cfgw(7, 0); cfgw(8, 0);
        m_fvalid = 0; m_count = 0;
    end
    endtask

    // random address from a mix of interesting places
    function [31:0] raddr;
        input dummy;
        integer k;
    begin
        k = $unsigned($random(seed)) % (14 + MAIN_512);
        case (k)
            0: raddr = 32'h8000_0000 + (($unsigned($random(seed)) % (SIZE + 32'h800)) & ~3);
            1: raddr = BASE + (($unsigned($random(seed)) % (SIZE + 32'h800)) & ~3);
            2: raddr = 32'h4000_0000 + (($unsigned($random(seed)) % 32'h20000) & ~3);
            3: raddr = ($unsigned($random(seed)) % 32'h2000) & ~3;
            4: raddr = GATE;
            5: raddr = 32'h10;
            6: raddr = 32'h7000_0000 + (($unsigned($random(seed)) % 32'h400) & ~3);
            7: raddr = 32'h7000_0218;
            8: raddr = 32'hB000_0000;
            14: raddr = 32'h5000_0000 + (($unsigned($random(seed)) % 32'h20000) & ~3);
            9: raddr = 32'h1F00_0000 + (($unsigned($random(seed)) % 32'h100) & ~3);
            10: raddr = 32'h2000_0000 + (($unsigned($random(seed)) % 32'h1000) & ~3);
            11: raddr = 32'hF000_0000 + (($unsigned($random(seed)) % 32'h400) & ~3);
            12: raddr = 32'h9000_0000;
            default: raddr = BASE - 4;
        endcase
    end
    endfunction

    integer n, k;
    reg [31:0] a;

    initial begin
        seed = SEED; errors = 0; passed_down = 0; lat = 0;
        adr = 0; dat = 0; we = 0; stb = 0; cyc = 0; instr = 0; sel = 0;
        d_ack = 0; d_dat = 0; mtu_base = 0;
        m_en = 0; m_enf = 0; m_priv = 1; m_count = 0; m_fvalid = 0;
        rst = 1; repeat (4) @(posedge clk); rst = 0;

        // -- disabled: everything passes, even "app" code
        cfgr(9);
        if (got !== 32'h3A50_0001) begin $display("ERROR: INFO %h", got); errors = errors + 1; end
        mtu_base = BASE;
        access(32'h8000_0000, 0, 1);
        access(32'h4002_0000, 1, 0);
        access(32'h0000_0010, 1, 0);
        check_fault;

        // -- program it (from kernel code: fetch something in text first)
        access(32'h4000_0400, 0, 1);
        cfgw(1, KTEXT); cfgw(2, GATE); cfgw(3, SIZE); cfgw(4, MASK);
        cfgw(0, 32'h7);                 // enable, enforce, irq
        m_en = 1; m_enf = 1;
        cfgr(1); if (got !== KTEXT) begin $display("ERROR: KTEXT readback"); errors = errors + 1; end

        // -- kernel code: anything goes
        access(32'h4000_0404, 0, 1);
        access(32'h4002_0000, 1, 0);
        access(32'h0000_0010, 1, 0);
        access(32'h7000_0218, 1, 0);
        access(32'hB000_0000, 0, 0);
        check_fault;

        // -- into the app
        access(32'h8000_0000, 0, 1);
        access(32'h8000_0100, 1, 0);                 // own, via window
        access(BASE + 32'h100, 1, 0);                // own, physical
        access(32'h8000_0000 + SIZE, 1, 0);          // just past the window
        check_fault;
        access(BASE + SIZE, 1, 0);                   // just past, physical
        access(32'h4002_0000, 1, 0);                 // kernel data
        access(32'h4002_0000, 0, 0);                 // ...readable
        access(32'h0000_0000, 1, 0);                 // null store
        access(32'h0000_000c, 0, 0);                 // reg_kernel read
        access(32'h7000_0218, 1, 0);                 // reconfigure key
        access(32'h7000_0200, 1, 0);                 // other socctl: ok
        access(32'h7000_0100, 1, 0);                 // cache control
        access(32'h1F00_0000, 1, 0);                 // flash control
        access(32'hB000_0000, 0, 0);                 // SD card: masked
        access(32'h2000_0000, 1, 0);                 // VRAM: ok
        access(32'hF000_0000, 1, 0);                 // UART: ok
        check_fault;

        // unprivileged write to the MPU itself: violation, no effect
        bus(CFG + 12, 1, 0, 32'h0);                  // SIZE <- 0 ?
        m_count = m_count + 1;
        cfgr(3);
        if (got !== SIZE) begin $display("ERROR: app changed SIZE"); errors = errors + 1; end

        access(32'h4000_0200, 0, 1);                 // mid-kernel: blocked
        access(32'h4009_0000, 0, 1);                 // another block
        access(32'h8000_0004, 0, 1);                 // own code: ok
        access(GATE, 0, 1);                          // syscall: privileged
        access(32'h4002_0000, 1, 0);                 // kernel may write
        access(32'h8000_0008, 0, 1);                 // return to app
        access(32'h10, 0, 1);                        // IRQ vector
        access(32'h0000_0100, 1, 0);                 // handler writes BRAM
        access(32'h8000_000c, 0, 1);
        check_fault;
        access(GATE, 0, 1);                          // kernel clears it
        clear_fault;
        check_fault;

        // -- random soak, enforcing, then report-only
        for (k = 0; k < 2; k = k + 1) begin
            if (k == 1) begin
                access(GATE, 0, 1);                  // become privileged
                cfgw(0, 32'h5);                      // enable, irq; no enforce
                m_enf = 0;
            end
            for (n = 0; n < NOPS; n = n + 1) begin
                a = raddr(0);
                if (a == CFG || a[31:8] == 24'h9000_01) a = 32'h9000_0000;
                case ($unsigned($random(seed)) % 3)
                    0: access(a, 0, 1);
                    1: access(a, 0, 0);
                    default: access(a, 1, 0);
                endcase
                if (($unsigned($random(seed)) % 50) == 0) begin
                    // back to kernel then app, like a syscall return
                    access(GATE, 0, 1);
                    if ($random(seed) & 1) access(32'h8000_0000, 0, 1);
                end
            end
            // leave via the gate so the checks below run privileged
            access(GATE, 0, 1);
            check_fault;
            clear_fault;
        end

        if (errors == 0) $display("PASS tb_mpu XLATE=%0d seed=%0d", XLATE, SEED);
        else $display("FAIL tb_mpu: %0d errors", errors);
        $finish;
    end

endmodule
