#include <cmath>
#include <cassert>
#include <fstream>
#include "matrix.hpp"
#include "scoring_function.hpp"
#include "array.hpp"
#include "receptor.hpp"

receptor::receptor(const path& p, const array<double, 3>& center, const array<double, 3>& size, const double granularity) :
	corner0(center - 0.5 * size),
	corner1(corner0 + size),
	granularity(granularity),
	granularity_inverse(1 / granularity),
	num_probes({{
		static_cast<size_t>(size[0] * granularity_inverse) + 2,
		static_cast<size_t>(size[1] * granularity_inverse) + 2,
		static_cast<size_t>(size[2] * granularity_inverse) + 2
	}}),
	num_probes_product(num_probes[0] * num_probes[1] * num_probes[2]),
	p_offset(scoring_function::n),
	maps(scoring_function::n)
{
	// Initialize necessary variables for constructing a receptor.
	atoms.reserve(5000); // A receptor typically consists of <= 5,000 atoms.

	// Initialize helper variables for parsing.
	string residue = "XXXX"; // Current residue sequence, used to track residue change, initialized to a dummy value.
	size_t residue_start = 0; // The starting atom of the current residue.
	string line;

	 // Start parsing.
	for (ifstream ifs(p); getline(ifs, line);)
	{
		const string record = line.substr(0, 6);
		if (record == "ATOM  " || record == "HETATM")
		{
			// Parse the residue sequence located at 1-based [23, 26].
			if ((line[25] != residue[3]) || (line[24] != residue[2]) || (line[23] != residue[1]) || (line[22] != residue[0])) // This line is the start of a new residue.
			{
				residue[3] = line[25];
				residue[2] = line[24];
				residue[1] = line[23];
				residue[0] = line[22];
				residue_start = atoms.size();
			}

			// Parse the line.
			atom a(line);

			// Harmonize an unsupported atom type to carbon.
			if (a.ad_unsupported())
			{
				a.ad = 2;
				a.xs = 0;
				a.rf = 0;
			}

			// Skip non-polar hydrogens.
			if (a.is_nonpolar_hydrogen())
				continue;

			// For a polar hydrogen, the bonded hetero atom must be a hydrogen bond donor.
			if (a.is_polar_hydrogen())
			{
				for (size_t i = atoms.size(); i > residue_start;)
				{
					atom& b = atoms[--i];
					if (b.is_hetero() && b.is_neighbor(a))
					{
						b.donorize();
						break;
					}
				}
				continue;
			}

			// For a hetero atom, its connected carbon atoms are no longer hydrophobic.
			if (a.is_hetero())
			{
				for (size_t i = atoms.size(); i > residue_start;)
				{
					atom& b = atoms[--i];
					if (!b.is_hetero() && b.is_neighbor(a))
					{
						b.dehydrophobicize();
					}
				}
			}
			// For a carbon atom, it is no longer hydrophobic when connected to a hetero atom.
			else
			{
				for (size_t i = atoms.size(); i > residue_start;)
				{
					const atom& b = atoms[--i];
					if (b.is_hetero() && b.is_neighbor(a))
					{
						a.dehydrophobicize();
						break;
					}
				}
			}

			// Save the atom if and only if its distance to its projection point on the box is within cutoff.
			double r2 = 0;
			for (size_t i = 0; i < 3; ++i)
			{
				if (a.coord[i] < corner0[i])
				{
					const double d = a.coord[i] - corner0[i];
					r2 += d * d;
				}
				else if (a.coord[i] > corner1[i])
				{
					const double d = a.coord[i] - corner1[i];
					r2 += d * d;
				}
			}
			if (r2 < scoring_function::cutoff_sqr)
			{
				atoms.push_back(move(a));
			}
		}
		else if (record == "TER   ")
		{
			residue = "XXXX";
		}
	}
}

bool receptor::within(const array<double, 3>& coord) const
{
	return corner0[0] <= coord[0] && coord[0] < corner1[0]
	    && corner0[1] <= coord[1] && coord[1] < corner1[1]
	    && corner0[2] <= coord[2] && coord[2] < corner1[2];
}

array<size_t, 3> receptor::index(const array<double, 3>& coord) const
{
	return
	{{
		static_cast<size_t>((coord[0] - corner0[0]) * granularity_inverse),
		static_cast<size_t>((coord[1] - corner0[1]) * granularity_inverse),
		static_cast<size_t>((coord[2] - corner0[2]) * granularity_inverse),
	}};
}

size_t receptor::index(const array<size_t, 3>& idx) const
{
	return num_probes[0] * (num_probes[1] * idx[2] + idx[1]) + idx[0];
}

void receptor::precalculate(const vector<size_t>& xs)
{
	const size_t nxs = xs.size();
	for (size_t t0 = 0; t0 < scoring_function::n; ++t0)
	{
		vector<size_t>& p = p_offset[t0];
		p.resize(nxs);
		for (size_t i = 0; i < nxs; ++i)
		{
			const size_t t1 = xs[i];
			p[i] = mp(t0, t1);
		}
	}
}

void receptor::populate(const vector<size_t>& xs, const size_t z, const scoring_function& sf)
{
	const size_t n = xs.size();
	const double z_coord = corner0[2] + granularity * z;
	const size_t z_offset = num_probes[0] * num_probes[1] * z;

	for (const auto& a : atoms)
	{
		assert(!a.is_hydrogen());
		const double dz = z_coord - a.coord[2];
		const double dz_sqr = dz * dz;
		const double dydx_sqr_ub = scoring_function::cutoff_sqr - dz_sqr;
		if (dydx_sqr_ub <= 0)
			continue;
		const double dydx_ub = sqrt(dydx_sqr_ub);
		const double y_lb = a.coord[1] - dydx_ub;
		const double y_ub = a.coord[1] + dydx_ub;
		const size_t y_beg = y_lb > corner0[1] ? (y_lb < corner1[1] ? static_cast<size_t>((y_lb - corner0[1]) * granularity_inverse)     : num_probes[1]) : 0;
		const size_t y_end = y_ub > corner0[1] ? (y_ub < corner1[1] ? static_cast<size_t>((y_ub - corner0[1]) * granularity_inverse) + 1 : num_probes[1]) : 0;
		const vector<size_t>& p = p_offset[a.xs];
		size_t zy_offset = z_offset + num_probes[0] * y_beg;
		double dy = corner0[1] + granularity * y_beg - a.coord[1];
		for (size_t y = y_beg; y < y_end; ++y, zy_offset += num_probes[0], dy += granularity)
		{
			const double dy_sqr = dy * dy;
			const double dx_sqr_ub = dydx_sqr_ub - dy_sqr;
			if (dx_sqr_ub <= 0)
				continue;
			const double dx_ub = sqrt(dx_sqr_ub);
			const double x_lb = a.coord[0] - dx_ub;
			const double x_ub = a.coord[0] + dx_ub;
			const size_t x_beg = x_lb > corner0[0] ? (x_lb < corner1[0] ? static_cast<size_t>((x_lb - corner0[0]) * granularity_inverse)     : num_probes[0]) : 0;
			const size_t x_end = x_ub > corner0[0] ? (x_ub < corner1[0] ? static_cast<size_t>((x_ub - corner0[0]) * granularity_inverse) + 1 : num_probes[0]) : 0;
			const double dzdy_sqr = dz_sqr + dy_sqr;
			size_t zyx_offset = zy_offset + x_beg;
			double dx = corner0[0] + granularity * x_beg - a.coord[0];
			for (size_t x = x_beg; x < x_end; ++x, ++zyx_offset, dx += granularity)
			{
				const double dx_sqr = dx * dx;
				const double r2 = dzdy_sqr + dx_sqr;
				if (r2 >= scoring_function::cutoff_sqr)
					continue;
				const size_t r_offset = static_cast<size_t>(sf.ns * r2);
				for (size_t i = 0; i < n; ++i)
				{
					maps[xs[i]][zyx_offset] += sf.e[p[i]][r_offset];
				}
			}
		}
	}
}
