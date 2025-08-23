class BinarySortTree {
    let data: T?;
    let lchild: BinarySortTree?;
    let rchild: BinarySortTree?;

    new() {
        data = null;
        lchild = null;
        rchild = null;
    }

    new(d: T) {
        data = d;
        lchild = null;
        rchild = null;
    }

    insert(val: T) {
        if data == null {
            data = val;
            return;
        }

        if val < data {
            if lchild == null {
                lchild = BinarySortTree.new(val);
            } else {
                lchild.insert(val);
            }
            return;
        }
        
        if val > data {
            if rchild == null {
                rchild = BinarySortTree.new(val);
            } else {
                rchild.insert(val);
            }
            return;
        }
    }

    find(val: T) -> BinarySortTree<T>? {
        if data == null {
            return null;
        }

        if data == val {
            return self;
        }

        if val < data {
            return lchild == null ? false : lchild.find(val);
        }

        if data < val {
            return rchild == null ? false : rchild.find(val);
        }

        return null;
    }

    remove(val: T) -> BinarySortTree<T>? {
        if data == null {
            return self;
        }

        if data < val {
            if rchild == null {
                return self;
            }

            rchild = rchild.remove(val);
            return self;
        } else if data > val {
            if lchild == null {
                return self;
            }

            lchild = lchild.remove(val);
            return self;
        }

        // 当前节点就是待删除的节点
        if rchild == null {
            return lchild;
        }

        if lchild == null {
            return rchild;
        }

        // 左右节点都不为null
        let r_min = rchild.find_min();
        data = r_min.data;
        rchild = rchild.remove(r_min.data);

        return self;
    }

    find_min() -> BinarySortTree<T>? {
        if data == null {
            return null; // 空树
        }
        return lchild == null ? self : lchild.find_min();
    }

    data -> T { return data; }

    to_string() -> String {
        if lchild == null && rchild == null {
            return "(%(data))";
        }
        return "(%(data): l = %(lchild == null ? "()" : lchild), r = %(rchild == null ? "()" : rchild))";
    }
}

if VM.is_main {
    import std.random for XorshiftRandom;

    let bst: BinarySortTree<i32> = BinarySortTree.new();
    
    let rand: Random = XorshiftRandom.new();

    let rm_val_index = rand.rand_i32(0, 9);
    let rm_val: i32? = null;
    for i in 0..10 {
        let val: i32 = rand.rand_i32(0, 1000);
        if i == rm_val_index {
            rm_val = val;
        }
        bst.insert(val);
    }

    System.print("bst:    %(bst)");

    System.print("rm %(rm_val)");
    let rm_res: BinarySortTree<i32>? = bst.remove(rm_val);
    System.print("bst:    %(bst)");
    System.print("rm_res: %(rm_res)");
}
