// Zeitlos SOC -- sample-rate arithmetic shared by rtl/audio.v (FSDIV)
// and rtl/socctl.v (DDMI audio's clock regeneration), for power-on
// values only. `include it INSIDE a module: these are constant
// functions, evaluated at elaboration. Software does the same
// arithmetic at run time: sw/common/zaudio.h, z_audio_fsdiv_for() and
// z_audio_ddmi_acr(). docs/audio.md, docs/ddmi_audio.md.

// Greatest common divisor. Euclid terminates in well under 64 steps
// for any pair of 32-bit numbers this sees.
function [31:0] ar_gcd;
	input [31:0] a;
	input [31:0] b;
	reg [31:0] x;
	reg [31:0] y;
	reg [31:0] t;
	integer k;
	begin
		x = a;
		y = b;
		for (k = 0; k < 64; k = k + 1) begin
			if (y != 0) begin
				t = x % y;
				x = y;
				y = t;
			end
		end
		ar_gcd = x;
	end
endfunction

// FSDIV for fs Hz at clk Hz: STEP / MOD = 128 fs / clk, reduced.
function [31:0] ar_step;
	input [31:0] fs;
	input [31:0] clk;
	begin
		ar_step = (128 * fs) / ar_gcd(128 * fs, clk);
	end
endfunction

function [31:0] ar_mod;
	input [31:0] fs;
	input [31:0] clk;
	begin
		ar_mod = clk / ar_gcd(128 * fs, clk);
	end
endfunction

// Audio clock regeneration for DDMI audio at a 25.2 MHz pixel clock:
// the sink rebuilds 128 fs as 25.2e6 * N / CTS. With the sample clock
// at clk * STEP / (128 MOD) and each frame sent k times,
//     N / CTS = 128 k fs / 25.2e6 = k STEP clk / (25.2e6 MOD)
// reduced, then scaled: to the recommended N for a standard rate (6144
// at 48 kHz, 6272 at 44.1 kHz, 4096 at 32 kHz), otherwise to near
// 128 fs / 1000. Exact by construction: the ratio is never rounded,
// only scaled. The (assumed) 40/21 is 48 MHz over 25.2 MHz: every
// board has these two clocks from one crystal.
// The N a source would normally use at fs Hz: the recommended values
// for the standard rates, so a sink sees the numbers it was tested
// with; 0 for any other rate.
function [31:0] ar_std_n;
	input [31:0] fs;
	begin
		case (fs)
			32000:  ar_std_n = 4096;
			44100:  ar_std_n = 6272;
			48000:  ar_std_n = 6144;
			88200:  ar_std_n = 12544;
			96000:  ar_std_n = 12288;
			176400: ar_std_n = 25088;
			192000: ar_std_n = 24576;
			default: ar_std_n = 0;
		endcase
	end
endfunction

// The scale factor j applied to the reduced N0 / CTS0: the standard N
// when the rate is a standard one and N0 divides it, otherwise the j
// that brings N nearest 128 fs / 1000.
function [31:0] ar_acr_j;
	input [31:0] n0;
	input [31:0] step;
	input [31:0] mod;
	input [31:0] k;
	input [31:0] clk;
	reg [31:0] fs;
	reg [31:0] std;
	reg [31:0] target;
	reg [31:0] j;
	begin
		fs = (k * (clk / 128) * step) / mod;	// clk is a multiple of 128
		std = ((fs * mod) == (k * (clk / 128) * step)) ? ar_std_n(fs) : 0;
		if (std != 0 && (std % n0) == 0) begin
			j = std / n0;
		end else begin
			target = (fs * 128) / 1000;
			j = (target + n0 / 2) / n0;
			if (j == 0) j = 1;
		end
		ar_acr_j = j;
	end
endfunction

function [31:0] ar_acr_n;
	input [31:0] step;
	input [31:0] mod;
	input [31:0] k;
	input [31:0] clk;
	reg [31:0] n0;
	reg [31:0] c0;
	reg [31:0] g;
	begin
		// clk / 25.2e6 as a fraction: 48e6 / 25.2e6 = 40 / 21
		n0 = k * step * (clk / 1200000);
		c0 = mod * 21;
		g = ar_gcd(n0, c0);
		n0 = n0 / g;
		ar_acr_n = n0 * ar_acr_j(n0, step, mod, k, clk);
	end
endfunction

function [31:0] ar_acr_cts;
	input [31:0] step;
	input [31:0] mod;
	input [31:0] k;
	input [31:0] clk;
	reg [31:0] n0;
	reg [31:0] c0;
	reg [31:0] g;
	begin
		n0 = k * step * (clk / 1200000);
		c0 = mod * 21;
		g = ar_gcd(n0, c0);
		n0 = n0 / g;
		c0 = c0 / g;
		ar_acr_cts = c0 * ar_acr_j(n0, step, mod, k, clk);
	end
endfunction
