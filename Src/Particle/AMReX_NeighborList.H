#ifndef NEIGHBOR_LIST_H_
#define NEIGHBOR_LIST_H_

#include <AMReX_Particles.H>
#include <AMReX_CudaContainers.H>

#include <thrust/reduce.h>

namespace amrex
{

template <class ParticleType>
struct Neighbors
{    
    struct iterator
    {
        AMREX_GPU_HOST_DEVICE
        iterator (int start, int stop, const unsigned int * nbor_list_ptr, const ParticleType* pstruct) 
            : m_index(start), m_stop(stop), m_nbor_list_ptr(nbor_list_ptr), m_pstruct(pstruct)   
        {}
        
        AMREX_GPU_HOST_DEVICE
        void operator++ () { ++m_index;; }
        
        AMREX_GPU_HOST_DEVICE
        bool operator!= (iterator const& rhs) const { return m_index < m_stop; }
        
        AMREX_GPU_HOST_DEVICE
        ParticleType operator* () const { return m_pstruct[m_nbor_list_ptr[m_index]];  }
        
    private:
        const ParticleType* m_pstruct;
        const unsigned int* m_nbor_list_ptr;
        int m_index;
        int m_stop;
    };
    
    AMREX_GPU_HOST_DEVICE
    iterator begin () const {
        return iterator(m_nbor_offsets_ptr[m_i], m_nbor_offsets_ptr[m_i+1], 
                        m_nbor_list_ptr, m_pstruct);
    }

    AMREX_GPU_HOST_DEVICE
    iterator end () const { 
        return iterator(m_nbor_offsets_ptr[m_i+1], m_nbor_offsets_ptr[m_i+1], 
                        m_nbor_list_ptr, m_pstruct);
    }

    AMREX_GPU_HOST_DEVICE    
    Neighbors (int i, const unsigned int *nbor_offsets_ptr, const unsigned int *nbor_list_ptr,
               const ParticleType* pstruct)
        : m_i(i),
          m_nbor_offsets_ptr(nbor_offsets_ptr),
          m_nbor_list_ptr(nbor_list_ptr),
          m_pstruct(pstruct)
    {}
    
private:

    const int m_i;
    const unsigned int * m_nbor_offsets_ptr;
    const unsigned int * m_nbor_list_ptr;
    const ParticleType * m_pstruct;
};

template <class ParticleType>
struct NeighborData
{
    NeighborData (const Gpu::ManagedDeviceVector<unsigned int>& offsets, 
                  const Gpu::ManagedDeviceVector<unsigned int>& list,
                  const ParticleType* pstruct)
        : m_nbor_offsets_ptr(offsets.dataPtr()),
          m_nbor_list_ptr(list.dataPtr()),
          m_pstruct(pstruct)
    {}
    
    AMREX_GPU_HOST_DEVICE
    amrex::Neighbors<ParticleType> getNeighbors (int i) const
    { return Neighbors<ParticleType>(i, m_nbor_offsets_ptr, m_nbor_list_ptr, m_pstruct); }
    
    const unsigned int * m_nbor_offsets_ptr;
    const unsigned int * m_nbor_list_ptr;
    const ParticleType * m_pstruct;
};

template <class ParticleType>
class NeighborList
{
public:

    template <class CheckPair>
    void build (const Gpu::ManagedDeviceVector<ParticleType>& vec,
                const amrex::Box& bx, const amrex::Geometry& geom,
                CheckPair check_pair)
    {
        BL_PROFILE("NeighborList::build");
        
        m_pstruct = vec.dataPtr();

        const auto dxi = geom.InvCellSizeArray();
        const auto plo = geom.ProbLoArray();
                
        const auto lo = amrex::lbound(bx);
        const auto hi = amrex::ubound(bx);

        const size_t np = vec.size();
        
        Gpu::ManagedDeviceVector<unsigned int> cells(np);
        unsigned int* pcell = cells.dataPtr();
        
        Gpu::ManagedDeviceVector<unsigned int> counts(bx.numPts(), 0);
        unsigned int* pcount = counts.dataPtr();
        
        Gpu::ManagedDeviceVector<unsigned int> offsets(bx.numPts() + 1, np);
        unsigned int* poffset = offsets.dataPtr();
    
        Gpu::ManagedDeviceVector<unsigned int> permutation(np);
        unsigned int* pperm = permutation.dataPtr();
        
        // First we build the cell list data structure
        
        m_pstruct = vec.dataPtr();
        AMREX_FOR_1D ( np, i,
        {
            int ix = (m_pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (m_pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (m_pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            
            unsigned int uix = amrex::min(nx,amrex::max(0,ix));
            unsigned int uiy = amrex::min(ny,amrex::max(0,iy));
            unsigned int uiz = amrex::min(nz,amrex::max(0,iz));
            pcell[i] = (uix * ny + uiy) * nz + uiz; 
            Cuda::Atomic::Add(&pcount[pcell[i]], 1u);
        });

        thrust::exclusive_scan(counts.begin(), counts.end(), offsets.begin());
        
        thrust::copy(offsets.begin(), offsets.end()-1, counts.begin());
        
        constexpr unsigned int max_unsigned_int = std::numeric_limits<unsigned int>::max();
        
        AMREX_FOR_1D ( np, i,
        {
            unsigned int index = Cuda::Atomic::Inc(&pcount[pcell[i]], max_unsigned_int);
            pperm[index] = i;
        });
    
        // Now count the number of neighbors for each particle
        
        Gpu::ManagedDeviceVector<unsigned int> nbor_counts(np);
        unsigned int* pnbor_counts = nbor_counts.dataPtr();
        
        AMREX_FOR_1D ( np, i,
        {
            int ix = (m_pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (m_pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (m_pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;
            
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            
            
            int count = 0;
            
            for (int ii = amrex::max(ix-1, 0); ii <= amrex::min(ix+1, nx-1); ++ii) {
                for (int jj = amrex::max(iy-1, 0); jj <= amrex::min(iy+1, ny-1); ++jj) {
                    for (int kk = amrex::max(iz-1, 0); kk <= amrex::min(iz+1, nz-1); ++kk) {
                        int index = (ii * ny + jj) * nz + kk;
                        for (int p = poffset[index]; p < poffset[index+1]; ++p) {
                            if (pperm[p] == i) continue;
                            if (check_pair(m_pstruct[i], m_pstruct[pperm[p]]))
                                count += 1;
                        }
                    }
                }
            }
            
            pnbor_counts[i] = count;
        });
    
        // Now we can allocate and build our neighbor list
        
        const size_t total_nbors = thrust::reduce(nbor_counts.begin(), nbor_counts.end());
        m_nbor_offsets.resize(np + 1, total_nbors);
        unsigned int* pnbor_offset = m_nbor_offsets.dataPtr();
        
        thrust::exclusive_scan(nbor_counts.begin(), nbor_counts.end(),
                               m_nbor_offsets.begin());
        
        m_nbor_list.resize(total_nbors);
        unsigned int* pm_nbor_list = m_nbor_list.dataPtr();
        
        AMREX_FOR_1D ( np, i,
        {
            int ix = (m_pstruct[i].pos(0)-plo[0])*dxi[0] - lo.x;
            int iy = (m_pstruct[i].pos(1)-plo[1])*dxi[1] - lo.y;
            int iz = (m_pstruct[i].pos(2)-plo[2])*dxi[2] - lo.z;
            
            int nx = hi.x-lo.x+1;
            int ny = hi.y-lo.y+1;
            int nz = hi.z-lo.z+1;            
            
            int n = 0;            
            for (int ii = amrex::max(ix-1, 0); ii <= amrex::min(ix+1, nx-1); ++ii) {
                for (int jj = amrex::max(iy-1, 0); jj <= amrex::min(iy+1, ny-1); ++jj) {
                    for (int kk = amrex::max(iz-1, 0); kk <= amrex::min(iz+1, nz-1); ++kk) {
                        int index = (ii * ny + jj) * nz + kk;
                        for (int p = poffset[index]; p < poffset[index+1]; ++p) {
                            if (pperm[p] == i) continue;
                            if (check_pair(m_pstruct[i], m_pstruct[pperm[p]])) {
                                pm_nbor_list[pnbor_offset[i] + n] = pperm[p]; 
                                ++n;
                            }
                        }
                    }
                }
            }
        });
    }

    NeighborData<ParticleType> data () 
    { 
        return NeighborData<ParticleType>(m_nbor_offsets, m_nbor_list, m_pstruct); 
    }

    int numParticles () { return m_nbor_offsets.size() - 1; }

    void print ()
    {
        BL_PROFILE("NeighborList::print");
        
        Gpu::HostVector<unsigned int> host_nbor_offsets(m_nbor_offsets.size());
        Gpu::HostVector<unsigned int> host_nbor_list(m_nbor_list.size());
        
        Cuda::thrust_copy(m_nbor_offsets.begin(),
                          m_nbor_offsets.end(),
                          host_nbor_offsets.begin());
        
        Cuda::thrust_copy(m_nbor_list.begin(),
                          m_nbor_list.end(),
                          host_nbor_list.begin());
        
        for (int i = 0; i < numParticles(); ++i) {
            amrex::Print() << "Particle " << i << " could collide with: ";
            for (int j = host_nbor_offsets[i]; j < host_nbor_offsets[i+1]; ++j) {
                amrex::Print() << host_nbor_list[j] << " ";
            }
            amrex::Print() << "\n";
        }
    }

protected:
    
    const ParticleType* m_pstruct;

    // This is the neighbor list data structure
    Gpu::ManagedDeviceVector<unsigned int> m_nbor_offsets;
    Gpu::ManagedDeviceVector<unsigned int> m_nbor_list;
};

}

#endif
