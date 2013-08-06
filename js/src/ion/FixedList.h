/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsion_ion_fixed_list_h__
#define jsion_ion_fixed_list_h__

namespace js {
namespace ion {

// List of a fixed length, but the length is unknown until runtime.
template <typename T>
class FixedList
{
    size_t length_;
    T *list_;

  private:
    FixedList(const FixedList&); // no copy definition.
    void operator= (const FixedList*); // no assignment definition.

  public:
    FixedList()
      : length_(0)
    { }

    // Dynamic memory allocation requires the ability to report failure.
    bool init(size_t length) {
        length_ = length;
        if (length == 0)
            return true;

        list_ = (T *)GetIonContext()->temp->allocate(length * sizeof(T));
        return list_ != NULL;
    }

    size_t length() const {
        return length_;
    }

    void shrink(size_t num) {
        JS_ASSERT(num < length_);
        length_ -= num;
    }

    bool growBy(size_t num) {
        T *list = (T *)GetIonContext()->temp->allocate((length_ + num) * sizeof(T));
        if (!list)
            return false;

        for (size_t i = 0; i < length_; i++)
            list[i] = list_[i];

        length_ += num;
        list_ = list;
        return true;
    }

    T &operator[](size_t index) {
        JS_ASSERT(index < length_);
        return list_[index];
    }
    const T &operator [](size_t index) const {
        JS_ASSERT(index < length_);
        return list_[index];
    }
};

} // namespace ion
} // namespace js

#endif // jsion_ion_fixed_list_h__

