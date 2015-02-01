#ifndef FORCE_H
#define FORCE_H

#include "h5_support.h"
#include <vector>
#include <memory>
#include <string>
#include "coord.h"
#include <functional>
#include <initializer_list>
#include <map>

struct DerivRecord {
    unsigned short atom, loc, output_width, unused;
    DerivRecord(unsigned short atom_, unsigned short loc_, unsigned short output_width_):
        atom(atom_), loc(loc_), output_width(output_width_) {}
};


struct SlotMachine
{
    const int width;
    const int n_elem;
    const int n_system;
    int offset;

    std::vector<DerivRecord> deriv_tape;
    std::vector<float>       accum;

    SlotMachine(int width_, int n_elem_, int n_system_): 
        width(width_), n_elem(n_elem_), n_system(n_system_), offset(0) {}

    void add_request(int output_width, CoordPair &pair) { 
        DerivRecord prev_record = deriv_tape.size() ? deriv_tape.back() : DerivRecord(-1,0,0);
        deriv_tape.emplace_back(pair.index, prev_record.loc+prev_record.output_width, output_width);
        pair.slot = deriv_tape.back().loc;
        for(int i=0; i<output_width*width*n_system; ++i) accum.push_back(0.f);
        offset += output_width*width;
    }

    SysArray accum_array() { return SysArray(accum.data(), offset); }
};


struct AutoDiffParams {
    unsigned char  n_slots1, n_slots2;
    unsigned short slots1[6];      
    unsigned short slots2[5];        

    AutoDiffParams(
            const std::initializer_list<unsigned short> &slots1_,
            const std::initializer_list<unsigned short> &slots2_)
    {
        unsigned loc1=0;
        for(auto i: slots1_) if(i!=(unsigned short)(-1)) slots1[loc1++] = i;
        n_slots1 = loc1;
        while(loc1<sizeof(slots1)/sizeof(slots1[0])) slots1[loc1++] = -1;

        unsigned loc2=0;
        for(auto i: slots2_) if(i!=(unsigned short)(-1)) slots2[loc2++] = i;
        n_slots2 = loc2;
        while(loc2<sizeof(slots2)/sizeof(slots2[0])) slots2[loc2++] = -1;
    }

    explicit AutoDiffParams(const std::initializer_list<unsigned short> &slots1_)
    { 
        unsigned loc1=0;
        for(auto i: slots1_) if(i!=(unsigned short)(-1)) slots1[loc1++] = i;
        n_slots1 = loc1;
        while(loc1<sizeof(slots1)/sizeof(slots1[0])) slots1[loc1++] = -1;

        unsigned loc2=0;
        // for(auto i: slots2_) if(i!=(unsigned short)(-1)) slots1[loc2++] = i;
        n_slots2 = loc2;
        while(loc2<sizeof(slots2)/sizeof(slots2[0])) slots2[loc2++] = -1;
    }

} ;  // struct for implementing reverse autodifferentiation

enum ComputeMode { DerivMode = 0, PotentialAndDerivMode = 1 };

struct DerivComputation 
{
    const bool potential_term;
    int n_system;
    DerivComputation(bool potential_term_, int n_system_):
        potential_term(potential_term_), n_system(n_system_) {}
    virtual void compute_value(ComputeMode mode)=0;
    virtual void propagate_deriv() =0;
    virtual double test_value_deriv_agreement() = 0;
};

struct CoordNode : public DerivComputation
{
    int n_elem;
    int elem_width;
    std::vector<float> output;
    SlotMachine slot_machine;
    CoordNode(int n_system_, int n_elem_, int elem_width_):
        DerivComputation(false, n_system_), n_elem(n_elem_), elem_width(elem_width_), 
        output(n_system*n_elem*elem_width), 
        slot_machine(elem_width, n_elem, n_system) {}
    virtual CoordArray coords() {
        return CoordArray(SysArray(output.data(), n_elem*elem_width), slot_machine.accum_array());
    }
};


struct PotentialNode : public DerivComputation
{
    std::vector<float> potential;
    PotentialNode(int n_system_):
        DerivComputation(true, n_system_), potential(n_system_) {}
    virtual void propagate_deriv() {};
};


struct HBondCounter : public PotentialNode {
    float n_hbond;
    HBondCounter(int n_system_): PotentialNode(n_system_), n_hbond(-1.f) {};
};


struct Pos : public CoordNode
{
    int n_atom;
    std::vector<float> deriv;

    Pos(int n_atom_, int n_system_):
        CoordNode(n_system_, n_atom_, 3), 
        n_atom(n_atom_), deriv(3*n_atom*n_system, 0.f)
    {}

    virtual void compute_value(ComputeMode mode) {};
    virtual void propagate_deriv();
    virtual double test_value_deriv_agreement() {return 0.;};
    CoordArray coords() {
        return CoordArray(SysArray(output.data(), n_atom*3), slot_machine.accum_array());
    }
};


struct DerivEngine
{
    struct Node 
    {
        std::string name;
        std::unique_ptr<DerivComputation> computation;
        std::vector<size_t> parents;  // cannot hold pointer to vector contents, so just store index
        std::vector<size_t> children;

        int germ_exec_level;
        int deriv_exec_level;

        Node(std::string name_, std::unique_ptr<DerivComputation>&& computation_):
            name(name_), computation(move(computation_)) {};
    };

    std::vector<Node> nodes;  // nodes[0] is the pos node
    Pos* pos;
    std::vector<float> potential;

    DerivEngine(int n_atom, int n_system): 
        potential(n_system)
    {
        nodes.emplace_back("pos", std::unique_ptr<DerivComputation>(new Pos(n_atom, n_system)));
        pos = dynamic_cast<Pos*>(nodes[0].computation.get());
    }

    void add_node(
            const std::string& name, 
            std::unique_ptr<DerivComputation>&& fcn, 
            std::vector<std::string> argument_names);

    Node& get(const std::string& name);
    int get_idx(const std::string& name, bool must_exist=true);

    template <typename T>
    T& get_computation(const std::string& name) {
        return dynamic_cast<T&>(*get(name).computation.get());
    }

    void compute(ComputeMode mode);
    enum IntegratorType {Verlet=0, Predescu=1};
    void integration_cycle(float* mom, float dt, float max_force, IntegratorType type = Verlet);
};

double get_n_hbond(DerivEngine &engine);
DerivEngine initialize_engine_from_hdf5(int n_atom, int n_system, hid_t potential_group, bool quiet=false);

// note that there are no null points in the vector of CoordNode*
typedef std::vector<CoordNode*> ArgList;
typedef std::function<DerivComputation*(hid_t, const ArgList&)> NodeCreationFunction;
typedef std::map<std::string, NodeCreationFunction> NodeCreationMap;
NodeCreationMap& node_creation_map(); 

bool is_prefix(const std::string& s1, const std::string& s2);
void add_node_creation_function(std::string name_prefix, NodeCreationFunction fcn);
void check_elem_width(const CoordNode& node, int expected_elem_width);
void check_arguments_length(const ArgList& arguments, int n_expected);

template <typename NodeClass, int n_args>
struct RegisterNodeType {
    RegisterNodeType(std::string name_prefix);
};

template <typename NodeClass>
struct RegisterNodeType<NodeClass,0> {
    RegisterNodeType(std::string name_prefix){
        NodeCreationFunction f = [](hid_t grp, const ArgList& args) {
            check_arguments_length(args,0); 
            return new NodeClass(grp);};
        add_node_creation_function(name_prefix, f);
    }
};

template <typename NodeClass>
struct RegisterNodeType<NodeClass,1> {
    RegisterNodeType(std::string name_prefix){
        NodeCreationFunction f = [](hid_t grp, const ArgList& args) {
            check_arguments_length(args,1); 
            return new NodeClass(grp, *args[0]);};
        add_node_creation_function(name_prefix, f);
    }
};

template <typename NodeClass>
struct RegisterNodeType<NodeClass,2> {
    RegisterNodeType(std::string name_prefix){
        NodeCreationFunction f = [](hid_t grp, const ArgList& args) {
            check_arguments_length(args,2); 
            return new NodeClass(grp, *args[0], *args[1]);};
        add_node_creation_function(name_prefix, f);
    }
};


template <int my_width, int width1, int width2>
void reverse_autodiff(
        const SysArray accum,
        SysArray deriv1,
        SysArray deriv2,
        const DerivRecord* tape,
        const AutoDiffParams* p,
        int n_tape,
        int n_atom, 
        int n_system)
{
#pragma omp parallel for
    for(int ns=0; ns<n_system; ++ns) {
        std::vector<TempCoord<my_width>> sens(n_atom);
        for(int nt=0; nt<n_tape; ++nt) {
            auto tape_elem = tape[nt];
            for(int rec=0; rec<tape_elem.output_width; ++rec) {
                auto val = StaticCoord<my_width>(accum, ns, tape_elem.loc + rec);
                for(int d=0; d<my_width; ++d)
                    sens[tape_elem.atom].v[d] += val.v[d];
            }
        }

        for(int na=0; na<n_atom; ++na) {
            if(width1) {
                for(int nsl=0; nsl<p[na].n_slots1; ++nsl) {
                    for(int sens_dim=0; sens_dim<my_width; ++sens_dim) {
                        MutableCoord<width1> c(deriv1, ns, p[na].slots1[nsl]+sens_dim);
                        for(int d=0; d<width1; ++d) c.v[d] *= sens[na].v[sens_dim];
                        c.flush();
                    }
                }
            }

            if(width2) {
                for(int nsl=0; nsl<p[na].n_slots2; ++nsl) {
                    for(int sens_dim=0; sens_dim<my_width; ++sens_dim) {
                        MutableCoord<width2> c(deriv2, ns, p[na].slots2[nsl]+sens_dim);
                        for(int d=0; d<width2; ++d) c.v[d] *= sens[na].v[sens_dim];
                        c.flush();
                    }
                }
            }
        }
    }
}


enum ValueType {CARTESIAN_VALUE=0, ANGULAR_VALUE=1, BODY_VALUE=2};

std::vector<float> central_difference_deriviative(
        const std::function<void()> &compute_value, std::vector<float> &input, std::vector<float> &output,
        float eps=1e-2f, ValueType value_type = CARTESIAN_VALUE);


template <int NDIM_INPUT>
std::vector<float> extract_jacobian_matrix( const std::vector<std::vector<CoordPair>>& coord_pairs,
        int elem_width_output, const std::vector<AutoDiffParams>* ad_params, 
        CoordNode &input_node, int n_arg)
{
    using namespace std;
    // First validate coord_pairs consistency with ad_params
    if(ad_params) {
        vector<unsigned short> slots;
        if(ad_params->size() != coord_pairs.size()) throw string("internal error");
        for(unsigned no=0; no<ad_params->size(); ++no) {
            slots.resize(0);
            auto p = (*ad_params)[no];
            if     (n_arg==0) slots.insert(begin(slots), p.slots1, p.slots1+p.n_slots1);
            else if(n_arg==1) slots.insert(begin(slots), p.slots2, p.slots2+p.n_slots2);
            else throw string("internal error");

            if(slots.size() != coord_pairs[no].size()) 
                throw string("size mismatch (") + to_string(slots.size()) + " != " + to_string(coord_pairs[no].size()) + ")";
            for(unsigned i=0; i<slots.size(); ++i) if(slots[i] != coord_pairs[no][i].slot) throw string("inconsistent");
        }
    }

    int output_size = coord_pairs.size()*elem_width_output;
    int input_size  = input_node.n_elem*NDIM_INPUT;
    // special case handling for rigid bodies, since torques have 3 elements but quats have 4
    if(input_node.elem_width != NDIM_INPUT && input_node.elem_width!=7) 
        throw string("dimension mismatch ") + to_string(input_node.elem_width) + " " + to_string(NDIM_INPUT);

    vector<float> jacobian(output_size * input_size,0.f);
    SysArray accum_array = input_node.coords().deriv;

    for(unsigned no=0; no<coord_pairs.size(); ++no) {
        for(auto cp: coord_pairs[no]) {
            for(int eo=0; eo<elem_width_output; ++eo) {
                StaticCoord<NDIM_INPUT> d(accum_array, 0, cp.slot+eo);
                for(int i=0; i<NDIM_INPUT; ++i) {
                    jacobian[no*elem_width_output*input_size + eo*input_size + cp.index*NDIM_INPUT + i] += d.v[i];
                }
            }
        }
    }

    return jacobian;
}

template <typename T>
void dump_matrix(int nrow, int ncol, const char* name, T matrix) {
    if(int(matrix.size()) != nrow*ncol) throw std::string("impossible matrix sizes");
    FILE* f = fopen(name, "w");
    for(int i=0; i<nrow; ++i) {
	for(int j=0; j<ncol; ++j)
	    fprintf(f, "%f ", matrix[i*ncol+j]);
	fprintf(f, "\n");
    }
    fclose(f);
}

#if !defined(IDENT_NAME)
#define IDENT_NAME atom
#endif
template <typename T>
std::vector<std::vector<CoordPair>> extract_pairs(const std::vector<T>& params, bool is_potential) {
    std::vector<std::vector<CoordPair>> coord_pairs;
    int n_slot = sizeof(params[0].IDENT_NAME)/sizeof(params[0].IDENT_NAME[0]);
    for(int ne=0; ne<int(params.size()); ++ne) {
        if(ne==0 || !is_potential) coord_pairs.emplace_back();
        for(int nsl=0; nsl<n_slot; ++nsl) {
            CoordPair s = params[ne].IDENT_NAME[nsl];
            if(s.slot != slot_t(-1)) coord_pairs.back().push_back(s);
        }
    }
    return coord_pairs;
}




static double relative_rms_deviation(
        const std::vector<float> &reference, const std::vector<float> &actual) {
    if(reference.size() != actual.size()) 
        throw std::string("impossible size mismatch ") + 
            std::to_string(reference.size()) + " " + std::to_string(actual.size());
    double diff_mag2  = 0.;
    double value1_mag2 = 0.;
    for(size_t i=0; i<reference.size(); ++i) {
        diff_mag2  += sqr(reference[i]-actual[i]);
        value1_mag2 += sqr(reference[i]);
    }
    return sqrt(diff_mag2/value1_mag2);
}


template <int NDIM_INPUT, typename T,ValueType arg_type = CARTESIAN_VALUE>
double compute_relative_deviation_for_node(
        T& node, 
        CoordNode& argument, 
        const std::vector<std::vector<CoordPair>> &coord_pairs,
        ValueType value_type = CARTESIAN_VALUE) {
    // FIXME handle multiple systems

    std::vector<float>& output = node.potential_term 
        ? reinterpret_cast<PotentialNode&>(node).potential
        : reinterpret_cast<CoordNode&>    (node).output;

    int elem_width_output = 1;
    if(!node.potential_term) elem_width_output = reinterpret_cast<CoordNode&>(node).elem_width;

    auto fd_deriv = central_difference_deriviative(
            [&](){node.compute_value(PotentialAndDerivMode);}, 
            argument.output, output, 1e-2, value_type);

    node.compute_value(DerivMode);
    auto pred_deriv = extract_jacobian_matrix<arg_type==BODY_VALUE ? 6 : NDIM_INPUT>(
            coord_pairs, elem_width_output, nullptr, argument, 0);

    if(arg_type == BODY_VALUE) {
        // we need to convert the torque to a quaternion derivative
        if(NDIM_INPUT != 7) throw "impossible";
        std::vector<float> pred_deriv_quat;

        if(pred_deriv.size()%6) throw "wrong";
        if(argument.elem_width != 7) throw "wrong";
        if((pred_deriv.size()/6) % argument.n_elem) throw "inconsistent";

        for(unsigned i=0; 6*i<pred_deriv.size(); ++i) {
            // just copy over CoM derivatives
            pred_deriv_quat.push_back(pred_deriv[6*i+0]);
            pred_deriv_quat.push_back(pred_deriv[6*i+1]);
            pred_deriv_quat.push_back(pred_deriv[6*i+2]);

            const float* q = argument.output.data() + 7*(i%argument.n_elem)+3;

            // rotate torque back to the reference frame
            float torque[3] = {pred_deriv[6*i+3], pred_deriv[6*i+4], pred_deriv[6*i+5]};
            pred_deriv_quat.push_back(2.f*(-torque[0]*q[1] - torque[1]*q[2] - torque[2]*q[3]));
            pred_deriv_quat.push_back(2.f*( torque[0]*q[0] + torque[1]*q[3] - torque[2]*q[2]));
            pred_deriv_quat.push_back(2.f*( torque[1]*q[0] + torque[2]*q[1] - torque[0]*q[3]));
            pred_deriv_quat.push_back(2.f*( torque[2]*q[0] + torque[0]*q[2] - torque[1]*q[1]));
        }

        pred_deriv = pred_deriv_quat;
    }
    return relative_rms_deviation(fd_deriv, pred_deriv);
}


#endif