#include <iomanip>
#include <random>
#include <numeric>
#include "utility.hpp"
#include "ligand.hpp"

void frame::output(boost::filesystem::ofstream& ofs) const
{
	ofs << "BRANCH"    << setw(4) << rotorXsrn << setw(4) << rotorYsrn << '\n';
}

ligand::ligand(const path& p) : nv(6)
{
	// Initialize necessary variables for constructing a ligand.
	frames.reserve(30); // A ligand typically consists of <= 30 frames.
	frames.push_back(frame(0, 0, 1, 0, 0)); // ROOT is also treated as a frame. The parent and rotorX of ROOT frame are dummy.
	atoms.reserve(100); // A ligand typically consists of <= 100 heavy atoms.

	// Initialize helper variables for parsing.
	vector<vector<size_t>> bonds; ///< Covalent bonds.
	bonds.reserve(100); // A ligand typically consists of <= 100 heavy atoms.
	size_t current = 0; // Index of current frame, initialized to ROOT frame.
	frame* f = &frames.front(); // Pointer to the current frame.
	string line;

	// Parse the ligand line by line.
	for (boost::filesystem::ifstream ifs(p); getline(ifs, line);)
	{
		const string record = line.substr(0, 6);
		if (record == "ATOM  " || record == "HETATM")
		{
			// Whenever an ATOM/HETATM line shows up, the current frame must be the last one.
			assert(current == frames.size() - 1);
			assert(f == &frames.back());

			// Parse the line.
			atom a(line);

			// Skip unsupported atom types.
			if (a.ad_unsupported()) continue;

			if (a.is_hydrogen()) // Current atom is a hydrogen.
			{
				for (size_t i = atoms.size(); i > f->beg;)
				{
					atom& b = atoms[--i];
					if (a.has_covalent_bond(b))
					{
						// For a polar hydrogen, the bonded hetero atom must be a hydrogen bond donor.
						if (a.is_polar_hydrogen())
						{
							assert(b.is_hetero());
							b.donorize();
						}
						else
						{
							assert(!b.is_hetero());
						}
						// Save the hydrogen.
						b.hydrogens.push_back(a);
						break;
					}
				}
			}
			else // Current atom is a heavy atom.
			{
				// Find bonds between the current atom and the other atoms of the same frame.
				assert(bonds.size() == atoms.size());
				bonds.push_back(vector<size_t>());
				bonds.back().reserve(4); // An atom typically consists of <= 4 bonds.
				for (size_t i = atoms.size(); i > f->beg;)
				{
					atom& b = atoms[--i];
					if (a.has_covalent_bond(b))
					{
						bonds[atoms.size()].push_back(i);
						bonds[i].push_back(atoms.size());

						// If carbon atom b is bonded to hetero atom a, b is no longer a hydrophobic atom.
						if (a.is_hetero() && !b.is_hetero())
						{
							b.dehydrophobicize();
						}
						// If carbon atom a is bonded to hetero atom b, a is no longer a hydrophobic atom.
						else if (!a.is_hetero() && b.is_hetero())
						{
							a.dehydrophobicize();
						}
					}
				}

				// Set rotorYidx if the serial number of current atom is rotorYsrn.
				if (a.serial == f->rotorYsrn)
				{
					f->rotorYidx = atoms.size();
					assert(f->rotorYidx == f->beg);
				}

				// Save the heavy atom.
				atoms.push_back(a);
			}
		}
		else if (record == "BRANCH")
		{
			// Parse "BRANCH   X   Y". X and Y are right-justified and 4 characters wide.
			const size_t rotorXsrn = stoul(line.substr( 6, 4));
			const size_t rotorYsrn = stoul(line.substr(10, 4));

			// Find the corresponding heavy atom with x as its atom serial number in the current frame.
			for (size_t i = f->beg; true; ++i)
			{
				if (atoms[i].serial == rotorXsrn)
				{
					// Insert a new frame whose parent is the current frame.
					frames.push_back(frame(current, rotorXsrn, rotorYsrn, i, atoms.size()));
					break;
				}
			}

			// The current frame has the newly inserted BRANCH frame as one of its branches.
			// It is unsafe to use f in place of frames[current] because frames could reserve a new memory block after calling push_back().
			frames[current].branches.push_back(frames.size() - 1);

			// Now the current frame is the newly inserted BRANCH frame.
			current = frames.size() - 1;

			// Update the pointer to the current frame.
			f = &frames[current];

			// The ending index of atoms of previous frame is the starting index of atoms of current frame.
			frames[current - 1].end = f->beg;
		}
		else if (record == "ENDBRA")
		{
			// A frame may be empty, e.g. "BRANCH   4   9" is immediately followed by "ENDBRANCH   4   9".
			// This emptiness is likely to be caused by invalid input structure, especially when all the atoms are located in the same plane.
			if (f->beg == atoms.size()) throw domain_error("Error parsing " + p.filename().string() + ": an empty BRANCH has been detected, indicating the input ligand structure is probably invalid.");

			// If the current frame consists of rotor Y and a few hydrogens only, e.g. -OH, -NH2 or -CH3,
			// the torsion of this frame will have no effect on scoring and is thus redundant.
			if (current + 1 == frames.size() && f->beg + 1 == atoms.size())
			{
				f->active = false;
			}
			else
			{
				++nv;
			}

			// Set up bonds between rotorX and rotorY.
			bonds[f->rotorYidx].push_back(f->rotorXidx);
			bonds[f->rotorXidx].push_back(f->rotorYidx);

			// Dehydrophobicize rotorX and rotorY if necessary.
			atom& rotorY = atoms[f->rotorYidx];
			atom& rotorX = atoms[f->rotorXidx];
			if (rotorY.is_hetero() && !rotorX.is_hetero()) rotorX.dehydrophobicize();
			if (rotorX.is_hetero() && !rotorY.is_hetero()) rotorY.dehydrophobicize();

			// Calculate yy and xy.
			const frame& p = frames[f->parent];
			f->yy = rotorY.coord - atoms[p.rotorYidx].coord;
			f->xy = normalize(rotorY.coord - rotorX.coord);

			// Now the parent of the following frame is the parent of current frame.
			current = f->parent;

			// Update the pointer to the current frame.
			f = &frames[current];
		}
	}
	assert(current == 0); // current should remain its original value if "BRANCH" and "ENDBRANCH" properly match each other.
	assert(f == &frames.front()); // The frame pointer should remain its original value if "BRANCH" and "ENDBRANCH" properly match each other.
	frames.back().end = na = atoms.size();
	nf = frames.size();
	assert(nf >= 1 + nv - 6);

	// Update atoms[].coord relative to frame origin.
	for (const frame& f : frames)
	{
		const array<float, 3> origin = atoms[f.rotorYidx].coord;
		for (size_t i = f.beg; i < f.end; ++i)
		{
			atom& a = atoms[i];
			a.coord -= origin;
			for (atom& h : a.hydrogens)
			{
				h.coord -= origin;
			}
		}
	}

	// Find intra-ligand interacting pairs that are not 1-4.
	interacting_pairs.reserve(na * na);
	vector<size_t> neighbors;
	neighbors.reserve(10); // An atom typically consists of <= 10 neighbors.
	for (size_t k1 = 0; k1 < nf; ++k1)
	{
		const frame& f1 = frames[k1];
		for (size_t i = f1.beg; i < f1.end; ++i)
		{
			// Find neighbor atoms within 3 consecutive covalent bonds.
			for (const size_t b1 : bonds[i])
			{
				if (find(neighbors.cbegin(), neighbors.cend(), b1) == neighbors.cend())
				{
					neighbors.push_back(b1);
				}
				for (const size_t b2 : bonds[b1])
				{
					if (find(neighbors.cbegin(), neighbors.cend(), b2) == neighbors.cend())
					{
						neighbors.push_back(b2);
					}
					for (const size_t b3 : bonds[b2])
					{
						if (find(neighbors.cbegin(), neighbors.cend(), b3) == neighbors.cend())
						{
							neighbors.push_back(b3);
						}
					}
				}
			}

			// Determine if interacting pairs can be possibly formed.
			const size_t t1 = atoms[i].xs;
			for (size_t k2 = k1 + 1; k2 < nf; ++k2)
			{
				const frame& f2 = frames[k2];
				const frame& f3 = frames[f2.parent];
				for (size_t j = f2.beg; j < f2.end; ++j)
				{
					if (k1 == f2.parent && (i == f2.rotorXidx || j == f2.rotorYidx)) continue;
					if (k1 > 0 && f1.parent == f2.parent && i == f1.rotorYidx && j == f2.rotorYidx) continue;
					if (f2.parent > 0 && k1 == f3.parent && i == f3.rotorXidx && j == f2.rotorYidx) continue;
					if (find(neighbors.cbegin(), neighbors.cend(), j) != neighbors.cend()) continue;
					interacting_pairs.push_back(interacting_pair(i, j, scoring_function::nr * mp(t1, atoms[j].xs)));
				}
			}

			// Clear the current neighbor set for the next atom.
			neighbors.clear();
		}
	}
	np = interacting_pairs.size();

	lig.resize(11 * nf + nf - 1 + 4 * na + 3 * np);
	int* c = lig.data();
	for (const frame& f : frames) *c++ = f.active;
	for (const frame& f : frames) *c++ = f.beg;
	for (const frame& f : frames) *c++ = f.end;
	for (const frame& f : frames) *c++ = f.branches.size();
	for (const frame& f : frames) *c++ = f.parent;
	for (const frame& f : frames) *(float*)c++ = f.yy[0];
	for (const frame& f : frames) *(float*)c++ = f.yy[1];
	for (const frame& f : frames) *(float*)c++ = f.yy[2];
	for (const frame& f : frames) *(float*)c++ = f.xy[0];
	for (const frame& f : frames) *(float*)c++ = f.xy[1];
	for (const frame& f : frames) *(float*)c++ = f.xy[2];
	assert(c == lig.data() + 11 * nf);
	for (const frame& f : frames)
	{
		for (const size_t b : f.branches)
		{
			*c++ = b;
		}
	}
	assert(c == lig.data() + 11 * nf + nf - 1);
	for (const atom& a : atoms) *(float*)c++ = a.coord[0];
	for (const atom& a : atoms) *(float*)c++ = a.coord[1];
	for (const atom& a : atoms) *(float*)c++ = a.coord[2];
	for (const atom& a : atoms) *c++ = a.xs;
	assert(c == lig.data() + 11 * nf + nf - 1 + 4 * na);
	for (const interacting_pair& p : interacting_pairs) *c++ = p.i0;
	for (const interacting_pair& p : interacting_pairs) *c++ = p.i1;
	for (const interacting_pair& p : interacting_pairs) *c++ = p.p_offset;
	assert(c == lig.data() + 11 * nf + nf - 1 + 4 * na + 3 * np);
	assert(c == &lig.back() + 1);
}

/// Represents a result found by BFGS local optimization for later clustering.
class solution
{
public:
//	float e; ///< Free energy.
//	vector<float> x; ///< Conformation vector.
	vector<array<float, 4>> q; ///< Frame quaternions.
	vector<array<float, 3>> c; ///< Heavy atom coordinates.
};

float ligand::save(const path& output_ligand_path, const vector<float>& ex, const size_t max_conformations, const size_t num_mc_tasks) const
{
	// Sort solutions in ascending order of e.
	vector<size_t> rank(num_mc_tasks);
	iota(rank.begin(), rank.end(), 0);
	sort(rank.begin(), rank.end(), [&ex](const size_t v0, const size_t v1)
	{
		return ex[v0] < ex[v1];
	});

	// Cluster solutions with RMSD of 2.0 and save them on the fly.
	const float square_deviation_threshold = 4.0f * na;
	vector<solution> solutions;
	solutions.reserve(max_conformations);
	boost::filesystem::ofstream ofs(output_ligand_path);
	ofs.setf(ios::fixed, ios::floatfield);
	ofs << setprecision(3);
	for (const size_t r : rank)
	{
		// Recover q and c from x.
		solution s;
		size_t o;
		s.q.resize(nf);
		s.c.resize(na);
		s.c[0][0] = ex[o = num_mc_tasks + r];
		s.c[0][1] = ex[o += num_mc_tasks];
		s.c[0][2] = ex[o += num_mc_tasks];
		s.q[0][0] = ex[o += num_mc_tasks];
		s.q[0][1] = ex[o += num_mc_tasks];
		s.q[0][2] = ex[o += num_mc_tasks];
		s.q[0][3] = ex[o += num_mc_tasks];
		for (size_t k = 0; k < nf; ++k)
		{
			const frame& f = frames[k];
			if (!f.active) continue;
			const array<float, 9> m = qtn4_to_mat3(s.q[k]);
			for (size_t i = f.beg + 1; i < f.end; ++i)
			{
				s.c[i] = s.c[f.beg] + m * atoms[i].coord;
			}
			for (const size_t i : f.branches)
			{
				const frame& b = frames[i];
				s.c[b.beg] = s.c[f.beg] + m * b.yy;
				if (!b.active) continue;
				const array<float, 3> a = m * b.xy;
				assert(normalized(a));
				s.q[i] = vec4_to_qtn4(a, ex[o += num_mc_tasks]) * s.q[k];
				assert(normalized(s.q[i]));
			}
		}

		// Check if c forms a new cluster.
		bool representative = true;
		for (const solution& t : solutions)
		{
			float square_deviation = 0.0f;
			for (size_t i = 0; i < na; ++i)
			{
				square_deviation += distance_sqr(s.c[i], t.c[i]);
			}
			if (square_deviation < square_deviation_threshold)
			{
				representative = false;
				break;
			}
		}
		if (!representative) continue;

		// Dump the ROOT frame.
		ofs << "ROOT\n";
		{
			const frame& f = frames.front();
			const array<float, 9> m = qtn4_to_mat3(s.q[0]);
			for (size_t i = f.beg; i < f.end; ++i)
			{
				const atom& a = atoms[i];
				a.output(ofs, s.c[i]);
				for (const atom& h : a.hydrogens)
				{
					h.output(ofs, s.c[f.beg] + m * h.coord);
				}
			}
		}
		ofs << "ENDROOT\n";

		// Dump the BRANCH frames.
		vector<bool> dumped(nf); // dump_branches[0] is dummy. The ROOT frame has been dumped.
		vector<size_t> stack; // Stack to track the depth-first traversal sequence of frames in order to avoid recursion.
		stack.reserve(nf - 1); // The ROOT frame is excluded.
		{
			const frame& f = frames.front();
			for (auto i = f.branches.rbegin(); i < f.branches.rend(); ++i)
			{
				stack.push_back(*i);
			}
		}
		while (!stack.empty())
		{
			const size_t fn = stack.back();
			const frame& f = frames[fn];
			if (dumped[fn]) // This BRANCH frame has been dumped.
			{
				ofs << "END";
				f.output(ofs);
				stack.pop_back();
			}
			else // This BRANCH frame has not been dumped.
			{
				f.output(ofs);
				const array<float, 9> m = qtn4_to_mat3(s.q[f.active ? fn : f.parent]);
				for (size_t i = f.beg; i < f.end; ++i)
				{
					const atom& a = atoms[i];
					a.output(ofs, s.c[i]);
					for (const atom& h : a.hydrogens)
					{
						h.output(ofs, s.c[f.beg] + m * h.coord);
					}
				}
				dumped[fn] = true;
				for (auto i = f.branches.rbegin(); i < f.branches.rend(); ++i)
				{
					stack.push_back(*i);
				}
			}
		}
		ofs << "TORSDOF " << nf - 1 << '\n';

		solutions.push_back(static_cast<solution&&>(s));
		if (solutions.size() == solutions.capacity()) break;
	}

	return ex[rank.front()];
}
