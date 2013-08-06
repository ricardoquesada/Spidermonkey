/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef Intl_h___
#define Intl_h___

#include "js/RootingAPI.h"

struct JSContext;
class JSObject;

/*
 * The Intl module specified by standard ECMA-402,
 * ECMAScript Internationalization API Specification.
 */

/**
 * Initializes the Intl Object and its standard built-in properties.
 * Spec: ECMAScript Internationalization API Specification, 8.0, 8.1
 */
extern JSObject *
js_InitIntlClass(JSContext *cx, js::HandleObject obj);


namespace js {

/*
 * The following functions are for use by self-hosted code.
 */


/******************** Collator ********************/

/**
 * Returns a new instance of the standard built-in Collator constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: collator = intl_Collator(locales, options)
 */
extern JSBool
intl_Collator(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns an object indicating the supported locales for collation
 * by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_Collator_availableLocales()
 */
extern JSBool
intl_Collator_availableLocales(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns an array with the collation type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * collations supported for the given locale. "standard" and "search" are
 * excluded.
 *
 * Usage: collations = intl_availableCollations(locale)
 */
extern JSBool
intl_availableCollations(JSContext *cx, unsigned argc, Value *vp);

/**
 * Compares x and y (which must be String values), and returns a number less
 * than 0 if x < y, 0 if x = y, or a number greater than 0 if x > y according
 * to the sort order for the locale and collation options of the given
 * Collator.
 *
 * Spec: ECMAScript Internationalization API Specification, 10.3.2.
 *
 * Usage: result = intl_CompareStrings(collator, x, y)
 */
extern JSBool
intl_CompareStrings(JSContext *cx, unsigned argc, Value *vp);


/******************** NumberFormat ********************/

/**
 * Returns a new instance of the standard built-in NumberFormat constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: numberFormat = intl_NumberFormat(locales, options)
 */
extern JSBool
intl_NumberFormat(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns an object indicating the supported locales for number formatting
 * by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_NumberFormat_availableLocales()
 */
extern JSBool
intl_NumberFormat_availableLocales(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns the numbering system type identifier per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * default numbering system for the given locale.
 *
 * Usage: defaultNumberingSystem = intl_numberingSystem(locale)
 */
extern JSBool
intl_numberingSystem(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns a string representing the number x according to the effective
 * locale and the formatting options of the given NumberFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 11.3.2.
 *
 * Usage: formatted = intl_FormatNumber(numberFormat, x)
 */
extern JSBool
intl_FormatNumber(JSContext *cx, unsigned argc, Value *vp);


/******************** DateTimeFormat ********************/

/**
 * Returns a new instance of the standard built-in DateTimeFormat constructor.
 * Self-hosted code cannot cache this constructor (as it does for others in
 * Utilities.js) because it is initialized after self-hosted code is compiled.
 *
 * Usage: dateTimeFormat = intl_DateTimeFormat(locales, options)
 */
extern JSBool
intl_DateTimeFormat(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns an object indicating the supported locales for date and time
 * formatting by having a true-valued property for each such locale with the
 * canonicalized language tag as the property name. The object has no
 * prototype.
 *
 * Usage: availableLocales = intl_DateTimeFormat_availableLocales()
 */
extern JSBool
intl_DateTimeFormat_availableLocales(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns an array with the calendar type identifiers per Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * supported calendars for the given locale. The default calendar is
 * element 0.
 *
 * Usage: calendars = intl_availableCalendars(locale)
 */
extern JSBool
intl_availableCalendars(JSContext *cx, unsigned argc, Value *vp);

/**
 * Return a pattern in the date-time format pattern language of Unicode
 * Technical Standard 35, Unicode Locale Data Markup Language, for the
 * best-fit date-time format pattern corresponding to skeleton for the
 * given locale.
 *
 * Usage: pattern = intl_patternForSkeleton(locale, skeleton)
 */
extern JSBool
intl_patternForSkeleton(JSContext *cx, unsigned argc, Value *vp);

/**
 * Returns a String value representing x (which must be a Number value)
 * according to the effective locale and the formatting options of the
 * given DateTimeFormat.
 *
 * Spec: ECMAScript Internationalization API Specification, 12.3.2.
 *
 * Usage: formatted = intl_FormatDateTime(dateTimeFormat, x)
 */
extern JSBool
intl_FormatDateTime(JSContext *cx, unsigned argc, Value *vp);

} // namespace js

#endif /* Intl_h___ */
