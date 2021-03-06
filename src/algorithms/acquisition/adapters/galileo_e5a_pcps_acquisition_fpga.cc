/*!
 * \file galileo_e5a_pcps_acquisition_fpga.cc
 * \brief Adapts a PCPS acquisition block to an AcquisitionInterface for
 *  Galileo E5a data and pilot Signals for the FPGA
 * \author Marc Majoral, 2019. mmajoral(at)cttc.es
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2019  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "galileo_e5a_pcps_acquisition_fpga.h"
#include "Galileo_E5a.h"
#include "configuration_interface.h"
#include "galileo_e5_signal_processing.h"
#include "gnss_sdr_flags.h"
#include <glog/logging.h>
#include <gnuradio/fft/fft.h>     // for fft_complex
#include <gnuradio/gr_complex.h>  // for gr_complex
#include <volk/volk.h>            // for volk_32fc_conjugate_32fc
#include <volk_gnsssdr/volk_gnsssdr.h>
#include <algorithm>  // for copy_n
#include <cmath>      // for abs, pow, floor
#include <complex>    // for complex

GalileoE5aPcpsAcquisitionFpga::GalileoE5aPcpsAcquisitionFpga(ConfigurationInterface* configuration,
    const std::string& role,
    unsigned int in_streams,
    unsigned int out_streams) : role_(role),
                                in_streams_(in_streams),
                                out_streams_(out_streams)
{
    pcpsconf_fpga_t acq_parameters;
    configuration_ = configuration;
    std::string default_dump_filename = "../data/acquisition.dat";

    DLOG(INFO) << "Role " << role;

    int64_t fs_in_deprecated = configuration_->property("GNSS-SDR.internal_fs_hz", 32000000);
    int64_t fs_in = configuration_->property("GNSS-SDR.internal_fs_sps", fs_in_deprecated);

    acq_parameters.repeat_satellite = configuration_->property(role + ".repeat_satellite", false);
    DLOG(INFO) << role << " satellite repeat = " << acq_parameters.repeat_satellite;

    uint32_t downsampling_factor = configuration_->property(role + ".downsampling_factor", 1);
    acq_parameters.downsampling_factor = downsampling_factor;
    fs_in = fs_in / downsampling_factor;

    acq_parameters.fs_in = fs_in;

    doppler_max_ = configuration_->property(role + ".doppler_max", 5000);
    if (FLAGS_doppler_max != 0) doppler_max_ = FLAGS_doppler_max;
    acq_parameters.doppler_max = doppler_max_;

    uint32_t sampled_ms = configuration_->property(role + ".coherent_integration_time_ms", 1);
    acq_parameters.sampled_ms = sampled_ms;

    acq_pilot_ = configuration_->property(role + ".acquire_pilot", false);
    acq_iq_ = configuration_->property(role + ".acquire_iq", false);
    if (acq_iq_)
        {
            acq_pilot_ = false;
        }

    auto code_length = static_cast<uint32_t>(std::round(static_cast<double>(fs_in) / GALILEO_E5A_CODE_CHIP_RATE_HZ * static_cast<double>(GALILEO_E5A_CODE_LENGTH_CHIPS)));
    acq_parameters.code_length = code_length;

    // The FPGA can only use FFT lengths that are a power of two.
    float nbits = ceilf(log2f((float)code_length * 2));
    uint32_t nsamples_total = pow(2, nbits);
    uint32_t select_queue_Fpga = configuration_->property(role + ".select_queue_Fpga", 1);
    acq_parameters.select_queue_Fpga = select_queue_Fpga;
    std::string default_device_name = "/dev/uio0";
    std::string device_name = configuration_->property(role + ".devicename", default_device_name);
    acq_parameters.device_name = device_name;
    acq_parameters.samples_per_ms = nsamples_total / sampled_ms;
    acq_parameters.samples_per_code = nsamples_total;

    acq_parameters.excludelimit = static_cast<unsigned int>(1 + ceil((1.0 / GALILEO_E5A_CODE_CHIP_RATE_HZ) * static_cast<float>(fs_in)));

    // compute all the GALILEO E5 PRN Codes (this is done only once in the class constructor in order to avoid re-computing the PRN codes every time
    // a channel is assigned)
    auto fft_if = std::unique_ptr<gr::fft::fft_complex>(new gr::fft::fft_complex(nsamples_total, true));  // Direct FFT
    std::vector<std::complex<float>> code(nsamples_total);                                                // buffer for the local code
    auto* fft_codes_padded = static_cast<gr_complex*>(volk_gnsssdr_malloc(nsamples_total * sizeof(gr_complex), volk_gnsssdr_get_alignment()));
    d_all_fft_codes_ = std::vector<uint32_t>(nsamples_total * GALILEO_E5A_NUMBER_OF_CODES);  // memory containing all the possible fft codes for PRN 0 to 32

    float max;  // temporary maxima search
    int32_t tmp, tmp2, local_code, fft_data;

    for (uint32_t PRN = 1; PRN <= GALILEO_E5A_NUMBER_OF_CODES; PRN++)
        {
            std::array<char, 3> signal_;
            signal_[0] = '5';
            signal_[2] = '\0';

            if (acq_iq_)
                {
                    signal_[1] = 'X';
                }
            else if (acq_pilot_)
                {
                    signal_[1] = 'Q';
                }
            else
                {
                    signal_[1] = 'I';
                }

            galileo_e5_a_code_gen_complex_sampled(code, signal_, PRN, fs_in, 0);

            for (uint32_t s = code_length; s < 2 * code_length; s++)
                {
                    code[s] = code[s - code_length];
                }

            // fill in zero padding
            for (uint32_t s = 2 * code_length; s < nsamples_total; s++)
                {
                    code[s] = std::complex<float>(0.0, 0.0);
                }

            std::copy_n(code.data(), nsamples_total, fft_if->get_inbuf());                     // copy to FFT buffer
            fft_if->execute();                                                                 // Run the FFT of local code
            volk_32fc_conjugate_32fc(fft_codes_padded, fft_if->get_outbuf(), nsamples_total);  // conjugate values

            max = 0;                                       // initialize maximum value
            for (uint32_t i = 0; i < nsamples_total; i++)  // search for maxima
                {
                    if (std::abs(fft_codes_padded[i].real()) > max)
                        {
                            max = std::abs(fft_codes_padded[i].real());
                        }
                    if (std::abs(fft_codes_padded[i].imag()) > max)
                        {
                            max = std::abs(fft_codes_padded[i].imag());
                        }
                }
            // map the FFT to the dynamic range of the fixed point values an copy to buffer containing all FFTs
            // and package codes in a format that is ready to be written to the FPGA
            for (uint32_t i = 0; i < nsamples_total; i++)
                {
                    tmp = static_cast<int32_t>(floor(fft_codes_padded[i].real() * (pow(2, quant_bits_local_code - 1) - 1) / max));
                    tmp2 = static_cast<int32_t>(floor(fft_codes_padded[i].imag() * (pow(2, quant_bits_local_code - 1) - 1) / max));
                    local_code = (tmp & select_lsbits) | ((tmp2 * shl_code_bits) & select_msbits);  // put together the real part and the imaginary part
                    fft_data = local_code & select_all_code_bits;
                    d_all_fft_codes_[i + (nsamples_total * (PRN - 1))] = fft_data;
                }
        }

    acq_parameters.all_fft_codes = d_all_fft_codes_.data();

    // reference for the FPGA FFT-IFFT attenuation factor
    acq_parameters.total_block_exp = configuration_->property(role + ".total_block_exp", 13);

    acq_parameters.num_doppler_bins_step2 = configuration_->property(role + ".second_nbins", 4);
    acq_parameters.doppler_step2 = configuration_->property(role + ".second_doppler_step", 125.0);
    acq_parameters.make_2_steps = configuration_->property(role + ".make_two_steps", false);
    acq_parameters.max_num_acqs = configuration_->property(role + ".max_num_acqs", 2);
    acquisition_fpga_ = pcps_make_acquisition_fpga(acq_parameters);

    channel_ = 0;
    doppler_step_ = 0;
    gnss_synchro_ = nullptr;

    // temporary buffers that we can release
    volk_gnsssdr_free(fft_codes_padded);
}


void GalileoE5aPcpsAcquisitionFpga::stop_acquisition()
{
    // this command causes the SW to reset the HW.
    acquisition_fpga_->reset_acquisition();
}


void GalileoE5aPcpsAcquisitionFpga::set_threshold(float threshold)
{
    DLOG(INFO) << "Channel " << channel_ << " Threshold = " << threshold;
    acquisition_fpga_->set_threshold(threshold);
}


void GalileoE5aPcpsAcquisitionFpga::set_doppler_max(unsigned int doppler_max)
{
    doppler_max_ = doppler_max;
    acquisition_fpga_->set_doppler_max(doppler_max_);
}


void GalileoE5aPcpsAcquisitionFpga::set_doppler_step(unsigned int doppler_step)
{
    doppler_step_ = doppler_step;
    acquisition_fpga_->set_doppler_step(doppler_step_);
}


void GalileoE5aPcpsAcquisitionFpga::set_doppler_center(int doppler_center)
{
    doppler_center_ = doppler_center;

    acquisition_fpga_->set_doppler_center(doppler_center_);
}


void GalileoE5aPcpsAcquisitionFpga::set_gnss_synchro(Gnss_Synchro* gnss_synchro)
{
    gnss_synchro_ = gnss_synchro;
    acquisition_fpga_->set_gnss_synchro(gnss_synchro_);
}


signed int GalileoE5aPcpsAcquisitionFpga::mag()
{
    return acquisition_fpga_->mag();
}


void GalileoE5aPcpsAcquisitionFpga::init()
{
    acquisition_fpga_->init();
}


void GalileoE5aPcpsAcquisitionFpga::set_local_code()
{
    acquisition_fpga_->set_local_code();
}


void GalileoE5aPcpsAcquisitionFpga::reset()
{
    acquisition_fpga_->set_active(true);
}


void GalileoE5aPcpsAcquisitionFpga::set_state(int state)
{
    acquisition_fpga_->set_state(state);
}


void GalileoE5aPcpsAcquisitionFpga::connect(gr::top_block_sptr top_block)
{
    if (top_block)
        { /* top_block is not null */
        };
    // Nothing to connect
}


void GalileoE5aPcpsAcquisitionFpga::disconnect(gr::top_block_sptr top_block)
{
    if (top_block)
        { /* top_block is not null */
        };
    // Nothing to disconnect
}


gr::basic_block_sptr GalileoE5aPcpsAcquisitionFpga::get_left_block()
{
    return nullptr;
}


gr::basic_block_sptr GalileoE5aPcpsAcquisitionFpga::get_right_block()
{
    return nullptr;
}
