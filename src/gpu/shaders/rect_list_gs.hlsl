// Xenos rectangle lists: each three vertices are three corners of a
// rectangle; the fourth is v1 + v2 - v0 for every output (as Xenia does).
//
// Translated vertex shaders write TEXCOORD0-7 (locations 0-7), COLOR0-1
// (8-9) and TEXCOORD8-15 (10-17); NG2's pixel shaders only read TEXCOORD0-7
// and COLOR0-1, and passing all 18 would exceed the 64 geometry input
// components some GPUs allow, so only those pass through. Each is its own
// variable at its location, as the vertex shader declares them (an array at
// locations 0-7 is matched element by element on most drivers, but not, it
// seems, on Intel's: there the sprites drawn as rectangles took their colour
// from the wrong slot, #18).
struct Vertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 t0 : TEXCOORD0;
    [[vk::location(1)]] float4 t1 : TEXCOORD1;
    [[vk::location(2)]] float4 t2 : TEXCOORD2;
    [[vk::location(3)]] float4 t3 : TEXCOORD3;
    [[vk::location(4)]] float4 t4 : TEXCOORD4;
    [[vk::location(5)]] float4 t5 : TEXCOORD5;
    [[vk::location(6)]] float4 t6 : TEXCOORD6;
    [[vk::location(7)]] float4 t7 : TEXCOORD7;
    [[vk::location(8)]] float4 color0 : COLOR0;
    [[vk::location(9)]] float4 color1 : COLOR1;
};

Vertex Fourth(Vertex corner, Vertex a, Vertex b)
{
    Vertex v;
    v.position = a.position + b.position - corner.position;
    v.t0 = a.t0 + b.t0 - corner.t0;
    v.t1 = a.t1 + b.t1 - corner.t1;
    v.t2 = a.t2 + b.t2 - corner.t2;
    v.t3 = a.t3 + b.t3 - corner.t3;
    v.t4 = a.t4 + b.t4 - corner.t4;
    v.t5 = a.t5 + b.t5 - corner.t5;
    v.t6 = a.t6 + b.t6 - corner.t6;
    v.t7 = a.t7 + b.t7 - corner.t7;
    v.color0 = a.color0 + b.color0 - corner.color0;
    v.color1 = a.color1 + b.color1 - corner.color1;
    return v;
}

[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> output)
{
    // The corner between the rectangle's two sides is the one opposite the
    // longest edge (the diagonal).
    float d01 = distance(input[0].position.xy / input[0].position.w, input[1].position.xy / input[1].position.w);
    float d02 = distance(input[0].position.xy / input[0].position.w, input[2].position.xy / input[2].position.w);
    float d12 = distance(input[1].position.xy / input[1].position.w, input[2].position.xy / input[2].position.w);
    uint corner = 0;
    if (d01 >= d02 && d01 >= d12)
        corner = 2;
    else if (d02 >= d01 && d02 >= d12)
        corner = 1;
    uint a = (corner + 1) % 3;
    uint b = (corner + 2) % 3;

    output.Append(input[a]);
    output.Append(input[corner]);
    output.Append(Fourth(input[corner], input[a], input[b]));
    output.Append(input[b]);
}
