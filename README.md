# NType

Way for efficient alphabet typing with n buttons by building n-ary Huffman tree.
Each button is binary: **click** vs **hold**.

Buttons are represented as the English letters `a`..`z`, so there is 26 buttons
limit which is exactly limitation for the convenience (Also i don't sure
that you even need ntype functionality having 26 buttons)
A code is a string of those letters: lowercase = click, uppercase = hold.

## Build

```sh
cc -Wall -Wextra -O2 -o ntype -Iinclude src/main.c src/utils.c
```

## Generate a tree

Symbols are comma-separated, **highest frequency first**:

```sh
./ntype gen "e,t,a,o,i,n,s,h,r,d,l,c,u,m,w,f,g,y,p,b,v,k,j,x,q,z"
./ntype gen -b 3 "e,t,a,o,i,n,s,h,r,d,l,c,u,m,w,f,g,y,p,b,v,k,j,x,q,z" ntype.json
```

`-b N` is how many physical buttons you have (default 1). Each step chooses
one of `2 * N` actions, on **leaf** reaching that symbol typed

JSON export (`tree` to walk while typing, `codes` to entype text):

```json
{
  "encoding": {
    "buttons": 2,
    "keys": "ab",
    "click": "lowercase",
    "hold": "uppercase"
  },
  "tree": { "symbol": null, "a": { "symbol": "e" }, "b": { "symbol": "t" } },
  "codes": [{ "symbol": "e", "seq": "a", "length": 1 }]
}
```

## X11 demo

Client example of usage ntype.json

```sh
./ntype gen -b 3 "e,t,a,o,i,n,s,h,r" ntype.json
cc -Wall -Wextra -O2 -o ntype-demo src/demo.c $(pkg-config --cflags --libs x11 xft)
./ntype-demo ntype.json
./ntype-demo --dump ntype.json   # print loaded codes, no window
```
